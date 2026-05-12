//
// Created by James Klappenbach on 4/19/23.
//

#include "MethodCallExpression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaStruct.h"
#include "cajeta/method/Method.h"
#include "Expression.h"
#include "DotExpression.h"
#include "Identifier.h"
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>

namespace cajeta {

    // Codegen dispatches three shapes:
    //   1. `arr.size()` on a CajetaArray receiver — structural accessor, loads the i64
    //      size field from the array header. Same shape will apply to every collection
    //      type per the project memory ("collections expose size() returning int64").
    //   2. `obj.foo(args)` with a class receiver — invokeMethod on the receiver's type.
    //   3. Bare `foo(args)` — resolves on the enclosing class with `this` as receiver.
    // Map a System.<stream> name to its POSIX file descriptor. Returns -1 if the
    // name isn't a recognized stream. `stderror` is accepted as a typo-friendly
    // alias for `stderr`.
    static int systemStreamFd(const std::string& name) {
        if (name == "stdout") return 1;
        if (name == "stderr" || name == "stderror") return 2;
        if (name == "stdin") return 0;
        return -1;
    }

    // Inspect children[0] for the shape `System.<stream>` — used by the intrinsic
    // path to lower `System.stdout.println(...)` and friends to direct runtime
    // calls without going through field/method resolution.
    static int detectSystemStreamReceiver(const AbstractSyntaxNodePtr& receiver) {
        auto dot = dynamic_pointer_cast<DotExpression>(receiver);
        if (!dot) return -1;
        int fd = systemStreamFd(dot->getIdentifier());
        if (fd < 0) return -1;
        const auto& dotChildren = const_cast<DotExpression*>(dot.get())->getChildren();
        if (dotChildren.empty()) return -1;
        auto sys = dynamic_pointer_cast<IdentifierExpression>(dotChildren[0]);
        if (!sys) return -1;
        if (sys->getTextValue() != "System") return -1;
        return fd;
    }

    // Lower an evaluated argument to the runtime ABI: returns a `ptr` regardless of
    // whether the expression resolved to a literal global string, a local pointer
    // variable, or some other pointer-typed value.
    static llvm::Value* loadStringArg(CajetaModulePtr module, const AbstractSyntaxNodePtr& argNode) {
        auto* builder = module->getBuilder();
        llvm::Value* v = argNode->generateCode(module);
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
            v = builder->CreateLoad(a->getAllocatedType(), a);
        }
        return v;
    }

    llvm::Value* MethodCallExpression::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& llvmCtx = *module->getLlvmContext();

        // ----- Struct view construction: `MyStruct(byte[] bytes)` -----
        // Synthesizes the view: bounds-check (data.size() >= sizeof(struct))
        // then GEP into the array header's data region and return a typed
        // pointer. The struct's "instance" is just that pointer; field
        // accesses GEP off it.
        //
        // Matches when the call is bare (no receiver) AND the method name is
        // the canonical name of a registered CajetaStruct.
        if (children.empty() && parameters.size() == 1) {
            auto structType = dynamic_pointer_cast<CajetaStruct>(
                CajetaType::of(methodCallName));
            if (structType) {
                // Evaluate the byte[] argument; load through if it's an alloca.
                llvm::Value* bytesPtr = parameters[0].expression->generateCode(module);
                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(bytesPtr)) {
                    bytesPtr = builder->CreateLoad(a->getAllocatedType(), a);
                }
                if (!bytesPtr) return nullptr;

                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);

                // Bounds check: byte_count = element_count * element_size must
                // be at least the struct's fixed-prefix size. We need the
                // element size of the byte-array argument, which lives on the
                // argument's resolved CajetaArray type. On failure we throw
                // via the existing exception runtime so user code can catch
                // it; an uncaught throw aborts, same as any other.
                uint64_t structBytes = structType->getFixedSize();
                uint64_t elemBytes = 1;  // sensible default if resolution fails
                if (auto argExpr = dynamic_pointer_cast<Expression>(parameters[0].expression)) {
                    if (!argExpr->getResolvedType()) argExpr->resolveTypes(module);
                    if (auto arrType = dynamic_pointer_cast<CajetaArray>(argExpr->getResolvedType())) {
                        if (auto elemTy = arrType->getElementLlvmType(&llvmCtx)) {
                            elemBytes = module->getLlvmModule()->getDataLayout().getTypeAllocSize(elemTy);
                            if (elemBytes == 0) elemBytes = 1;
                        }
                    }
                }

                llvm::Value* count = builder->CreateLoad(i64Ty, bytesPtr, "view_buf_count");
                llvm::Value* haveBytes = builder->CreateMul(count,
                    llvm::ConstantInt::get(i64Ty, elemBytes), "view_buf_bytes");
                llvm::Value* ok = builder->CreateICmpUGE(haveBytes,
                    llvm::ConstantInt::get(i64Ty, structBytes), "view_size_ok");

                llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
                llvm::BasicBlock* failBB = llvm::BasicBlock::Create(
                    llvmCtx, "view_size_fail", parentFn);
                llvm::BasicBlock* okBB = llvm::BasicBlock::Create(
                    llvmCtx, "view_size_ok", parentFn);
                builder->CreateCondBr(ok, okBB, failBB);

                builder->SetInsertPoint(failBB);
                if (llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw")) {
                    // Throw value is informational. Higher byte = struct-view-fail tag;
                    // low bits = needed minimum bytes (truncated). Catchers can
                    // currently only observe the value via __cajeta_get_thrown.
                    uint64_t tag = (uint64_t) 0xCA1E7A00 | (structBytes & 0xFF);
                    builder->CreateCall(throwFn,
                        {llvm::ConstantInt::get(i64Ty, tag)});
                }
                builder->CreateUnreachable();

                builder->SetInsertPoint(okBB);
                // GEP past the array header's i64 size field to reach data[0].
                llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                    i8Ty, bytesPtr,
                    llvm::ConstantInt::get(i64Ty, 8), "view_data_ptr");
                return dataPtr;
            }
        }

        // ----- System.<stream>.<method>(...) intrinsic -----
        if (!children.empty()) {
            int streamFd = detectSystemStreamReceiver(children[0]);
            if (streamFd >= 0) {
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);

                llvm::Value* streamArg = llvm::ConstantInt::get(i32Ty, streamFd);
                if ((methodCallName == "print" || methodCallName == "println")
                        && parameters.size() == 1) {
                    // Dispatch on the argument's LLVM type. Pointers (including String)
                    // hit the C-string helper; integers widen to i64; floats to f64;
                    // i1 (bool) routes to the dedicated boolean helper.
                    llvm::Value* arg = loadStringArg(module, parameters[0].expression);
                    llvm::Type* argTy = arg->getType();
                    const std::string base = methodCallName == "println"
                        ? "__cajeta_println" : "__cajeta_print";
                    llvm::Function* fn = nullptr;
                    if (argTy->isPointerTy()) {
                        fn = module->getRuntimeFunction(base);
                    } else if (argTy->isIntegerTy(1)) {
                        // i1 needs to widen to i32 to match the ABI of the bool helper.
                        arg = builder->CreateZExt(arg, i32Ty);
                        fn = module->getRuntimeFunction(base + "_bool");
                    } else if (argTy->isIntegerTy()) {
                        arg = builder->CreateIntCast(arg, i64Ty, /*isSigned=*/true);
                        fn = module->getRuntimeFunction(base + "_i64");
                    } else if (argTy->isFloatingPointTy()) {
                        llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                        if (argTy != f64Ty) {
                            arg = builder->CreateFPCast(arg, f64Ty);
                        }
                        fn = module->getRuntimeFunction(base + "_f64");
                    }
                    if (fn) return builder->CreateCall(fn, {streamArg, arg});
                }
                if (methodCallName == "printf" && parameters.size() >= 2) {
                    // printf(fmt, String[] args) lowering: pass (fd, fmt, size, &data[0]).
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_log");
                    if (!fn) return nullptr;
                    llvm::Value* fmt = loadStringArg(module, parameters[0].expression);
                    // The args array: resolve to header pointer, load size, GEP to data[0].
                    auto argsExpr = dynamic_pointer_cast<Expression>(parameters[1].expression);
                    llvm::Value* argsHdr = parameters[1].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(argsHdr)) {
                        argsHdr = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    CajetaArrayPtr arrType;
                    if (argsExpr) {
                        if (!argsExpr->getResolvedType()) argsExpr->resolveTypes(module);
                        arrType = dynamic_pointer_cast<CajetaArray>(argsExpr->getResolvedType());
                    }
                    if (!arrType) return nullptr;
                    llvm::Type* hdrTy = arrType->getLlvmType();
                    llvm::Value* sizePtr = builder->CreateStructGEP(hdrTy, argsHdr,
                        CajetaArray::SIZE_FIELD_INDEX);
                    llvm::Value* count = builder->CreateLoad(i64Ty, sizePtr);
                    llvm::Value* dataPtr = builder->CreateStructGEP(hdrTy, argsHdr,
                        CajetaArray::DATA_FIELD_INDEX);
                    return builder->CreateCall(fn, {streamArg, fmt, count, dataPtr});
                }
                // Unknown method or arity on a System stream; fall through to the
                // normal method-call path (which will surface a clearer error than
                // misrouting).
            }
        }

        // ----- Math.<fn>(...) intrinsic -----
        // Math acts as a static-only namespace today (no instance, no class file). We
        // recognize the literal identifier `Math` as receiver and lower each call to a
        // matching LLVM intrinsic — no extra runtime helpers needed.
        if (!children.empty()) {
            auto mathId = dynamic_pointer_cast<IdentifierExpression>(children[0]);
            if (mathId && mathId->getTextValue() == "Math") {
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                auto loadArg = [&](size_t i) -> llvm::Value* {
                    llvm::Value* v = parameters[i].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                        v = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    return v;
                };
                auto toF64 = [&](llvm::Value* v) {
                    if (v->getType()->isIntegerTy()) return builder->CreateSIToFP(v, f64Ty);
                    if (v->getType() != f64Ty) return builder->CreateFPCast(v, f64Ty);
                    return v;
                };
                auto toI64 = [&](llvm::Value* v) {
                    if (v->getType()->isIntegerTy()) {
                        return v->getType() == i64Ty
                            ? v : builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                    }
                    return v;
                };
                llvm::Module* lm = module->getLlvmModule();
                if (methodCallName == "abs" && parameters.size() == 1) {
                    llvm::Value* x = loadArg(0);
                    if (x->getType()->isFloatingPointTy()) {
                        llvm::Function* fn = llvm::Intrinsic::getDeclaration(
                            lm, llvm::Intrinsic::fabs, {x->getType()});
                        return builder->CreateCall(fn, {x});
                    }
                    x = toI64(x);
                    llvm::Function* fn = llvm::Intrinsic::getDeclaration(
                        lm, llvm::Intrinsic::abs, {i64Ty});
                    return builder->CreateCall(fn, {x, llvm::ConstantInt::getFalse(llvmCtx)});
                }
                if ((methodCallName == "max" || methodCallName == "min")
                        && parameters.size() == 2) {
                    llvm::Value* a = loadArg(0);
                    llvm::Value* b = loadArg(1);
                    bool isFp = a->getType()->isFloatingPointTy()
                             || b->getType()->isFloatingPointTy();
                    if (isFp) {
                        a = toF64(a);
                        b = toF64(b);
                        llvm::Intrinsic::ID id = methodCallName == "max"
                            ? llvm::Intrinsic::maxnum : llvm::Intrinsic::minnum;
                        llvm::Function* fn = llvm::Intrinsic::getDeclaration(lm, id, {f64Ty});
                        return builder->CreateCall(fn, {a, b});
                    }
                    a = toI64(a);
                    b = toI64(b);
                    llvm::Intrinsic::ID id = methodCallName == "max"
                        ? llvm::Intrinsic::smax : llvm::Intrinsic::smin;
                    llvm::Function* fn = llvm::Intrinsic::getDeclaration(lm, id, {i64Ty});
                    return builder->CreateCall(fn, {a, b});
                }
                if (methodCallName == "sqrt" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getDeclaration(
                        lm, llvm::Intrinsic::sqrt, {f64Ty});
                    return builder->CreateCall(fn, {x});
                }
                if (methodCallName == "pow" && parameters.size() == 2) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Value* y = toF64(loadArg(1));
                    llvm::Function* fn = llvm::Intrinsic::getDeclaration(
                        lm, llvm::Intrinsic::pow, {f64Ty});
                    return builder->CreateCall(fn, {x, y});
                }
                if (methodCallName == "floor" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getDeclaration(
                        lm, llvm::Intrinsic::floor, {f64Ty});
                    return builder->CreateCall(fn, {x});
                }
                if (methodCallName == "ceil" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getDeclaration(
                        lm, llvm::Intrinsic::ceil, {f64Ty});
                    return builder->CreateCall(fn, {x});
                }
                if (methodCallName == "round" && parameters.size() == 1) {
                    // Match Java's Math.round(double) → long: half-up rounding to i64.
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getDeclaration(
                        lm, llvm::Intrinsic::round, {f64Ty});
                    llvm::Value* rounded = builder->CreateCall(fn, {x});
                    return builder->CreateFPToSI(rounded, i64Ty);
                }
                // Single-arg transcendentals — all take/return double.
                struct UnaryFn { const char* name; llvm::Intrinsic::ID id; };
                static const UnaryFn unaryFns[] = {
                    {"sin",   llvm::Intrinsic::sin},
                    {"cos",   llvm::Intrinsic::cos},
                    {"log",   llvm::Intrinsic::log},     // natural log
                    {"log10", llvm::Intrinsic::log10},
                    {"exp",   llvm::Intrinsic::exp},
                };
                for (const auto& u : unaryFns) {
                    if (methodCallName == u.name && parameters.size() == 1) {
                        llvm::Value* x = toF64(loadArg(0));
                        llvm::Function* fn = llvm::Intrinsic::getDeclaration(lm, u.id, {f64Ty});
                        return builder->CreateCall(fn, {x});
                    }
                }
                // tan has no direct intrinsic in LLVM 18 — emit sin/cos division.
                if (methodCallName == "tan" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* sinFn = llvm::Intrinsic::getDeclaration(
                        lm, llvm::Intrinsic::sin, {f64Ty});
                    llvm::Function* cosFn = llvm::Intrinsic::getDeclaration(
                        lm, llvm::Intrinsic::cos, {f64Ty});
                    return builder->CreateFDiv(
                        builder->CreateCall(sinFn, {x}),
                        builder->CreateCall(cosFn, {x}));
                }
            }
        }

        // ----- Integer/Long/Double/Boolean/String static-namespace intrinsics -----
        // These wrap the C-side parse/format helpers so user code can write the
        // familiar Integer.parseInt(s) / String.valueOf(x) idioms without going
        // through real class dispatch.
        if (!children.empty()) {
            auto idExpr = dynamic_pointer_cast<IdentifierExpression>(children[0]);
            if (idExpr) {
                const std::string& ns = idExpr->getTextValue();
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                auto loadStr = [&](size_t i) {
                    return loadStringArg(module, parameters[i].expression);
                };
                auto loadValue = [&](size_t i) {
                    llvm::Value* v = parameters[i].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                        v = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    return v;
                };
                // Cajeta.* — language-internal diagnostics. Today: drop-chain
                // observability for the rollout's test suite. These are part of
                // the runtime, not the user-facing standard library.
                if (ns == "Cajeta" && methodCallName == "dropCount" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_drop_count_get");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "dropCountReset" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_drop_count_reset");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "System" && methodCallName == "exit" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_exit");
                    llvm::Value* code = loadValue(0);
                    if (code->getType()->isIntegerTy() && code->getType() != i32Ty) {
                        code = builder->CreateIntCast(code, i32Ty, /*isSigned=*/true);
                    }
                    return builder->CreateCall(fn, {code});
                }
                if (ns == "System" && methodCallName == "currentTimeMillis"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_currentTimeMillis");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Math" && methodCallName == "random" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_random");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Integer" && methodCallName == "parseInt" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_parse_i64");
                    llvm::Value* call = builder->CreateCall(fn, {loadStr(0)});
                    // Java's Integer.parseInt returns int — narrow our i64 to i32.
                    return builder->CreateIntCast(call, i32Ty, /*isSigned=*/true);
                }
                if (ns == "Long" && methodCallName == "parseLong" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_parse_i64");
                    return builder->CreateCall(fn, {loadStr(0)});
                }
                if (ns == "Double" && methodCallName == "parseDouble" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_parse_f64");
                    return builder->CreateCall(fn, {loadStr(0)});
                }
                // Integer/Long bit operations — all single-argument llvm.<op> calls.
                // Cajeta's `int` is i32, `long` is i64; we widen/narrow as needed.
                if ((ns == "Integer" || ns == "Long") && parameters.size() == 1
                        && (methodCallName == "bitCount"
                         || methodCallName == "numberOfLeadingZeros"
                         || methodCallName == "numberOfTrailingZeros"
                         || methodCallName == "reverse")) {
                    bool isLong = (ns == "Long");
                    llvm::Type* opTy = isLong ? i64Ty : i32Ty;
                    llvm::Value* x = loadValue(0);
                    if (x->getType() != opTy && x->getType()->isIntegerTy()) {
                        x = builder->CreateIntCast(x, opTy, /*isSigned=*/true);
                    }
                    llvm::Module* lm = module->getLlvmModule();
                    llvm::Intrinsic::ID id;
                    bool needsZeroFlag = false;
                    if (methodCallName == "bitCount") {
                        id = llvm::Intrinsic::ctpop;
                    } else if (methodCallName == "numberOfLeadingZeros") {
                        id = llvm::Intrinsic::ctlz;
                        needsZeroFlag = true;
                    } else if (methodCallName == "numberOfTrailingZeros") {
                        id = llvm::Intrinsic::cttz;
                        needsZeroFlag = true;
                    } else {
                        id = llvm::Intrinsic::bitreverse;
                    }
                    llvm::Function* fn = llvm::Intrinsic::getDeclaration(lm, id, {opTy});
                    llvm::Value* call;
                    if (needsZeroFlag) {
                        // false = return bit-width when input is zero (Java's behavior),
                        // rather than poison.
                        call = builder->CreateCall(fn,
                            {x, llvm::ConstantInt::getFalse(llvmCtx)});
                    } else {
                        call = builder->CreateCall(fn, {x});
                    }
                    // bitCount / numberOfLeading/TrailingZeros — Java returns int.
                    // reverse keeps the original width.
                    if (methodCallName != "reverse") {
                        if (call->getType() != i32Ty) {
                            call = builder->CreateIntCast(call, i32Ty, /*isSigned=*/true);
                        }
                    }
                    return call;
                }
                if (ns == "Boolean" && methodCallName == "parseBoolean" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_parse_bool");
                    llvm::Value* call = builder->CreateCall(fn, {loadStr(0)});
                    return builder->CreateICmpNE(call,
                        llvm::ConstantInt::get(call->getType(), 0));
                }
                // Integer.toString(int) / Long.toString(long) / Double.toString(double) /
                // Boolean.toString(bool) — same lowering as String.valueOf(...).
                if ((ns == "Integer" || ns == "Long" || ns == "Double" || ns == "Boolean")
                        && methodCallName == "toString" && parameters.size() == 1) {
                    llvm::Value* v = loadValue(0);
                    llvm::Type* t = v->getType();
                    if (ns == "Boolean" || t->isIntegerTy(1)) {
                        if (t->isIntegerTy(1)) v = builder->CreateZExt(v, i32Ty);
                        else if (t != i32Ty && t->isIntegerTy()) v = builder->CreateIntCast(v, i32Ty, true);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                        // bool_to_str returns const char*; cast away const for the IR side.
                        return builder->CreateCall(fn, {v});
                    }
                    if (t->isFloatingPointTy() || ns == "Double") {
                        if (t != f64Ty) v = t->isIntegerTy()
                            ? builder->CreateSIToFP(v, f64Ty)
                            : builder->CreateFPCast(v, f64Ty);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                        return builder->CreateCall(fn, {v});
                    }
                    // Integer/Long path.
                    if (t->isIntegerTy() && t != i64Ty) v = builder->CreateIntCast(v, i64Ty, true);
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                    return builder->CreateCall(fn, {v});
                }
                if (ns == "String" && methodCallName == "valueOf" && parameters.size() == 1) {
                    llvm::Value* v = loadValue(0);
                    llvm::Type* t = v->getType();
                    if (t->isPointerTy()) return v;  // already a String
                    if (t->isIntegerTy(1)) {
                        v = builder->CreateZExt(v, i32Ty);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                        return builder->CreateCall(fn, {v});
                    }
                    if (t->isIntegerTy(8)) {
                        // Treat i8 as char for String.valueOf — single-byte string.
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_fromChar");
                        return builder->CreateCall(fn, {v});
                    }
                    if (t->isIntegerTy()) {
                        if (t != i64Ty) v = builder->CreateIntCast(v, i64Ty, true);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                        return builder->CreateCall(fn, {v});
                    }
                    if (t->isFloatingPointTy()) {
                        if (t != f64Ty) v = builder->CreateFPCast(v, f64Ty);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                        return builder->CreateCall(fn, {v});
                    }
                }
            }
        }

        // Determine receiver (if any) from children[0]; the lhs is added when this node
        // was constructed via the DOT-methodCall branch.
        llvm::Value* receiver = nullptr;
        CajetaTypePtr receiverType;
        if (!children.empty()) {
            receiver = children[0]->generateCode(module);
            auto exprChild = dynamic_pointer_cast<Expression>(children[0]);
            if (exprChild) {
                if (!exprChild->getResolvedType()) {
                    exprChild->resolveTypes(module);
                }
                receiverType = exprChild->getResolvedType();
            }
            // l-value -> r-value coercion. Local-variable receivers are AllocaInsts;
            // ArrayIndex receivers are slot addresses where the slot holds a `ptr` to
            // the referenced object (CajetaArray inner header or class instance).
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(receiver)) {
                receiver = builder->CreateLoad(a->getAllocatedType(), a);
            } else if (dynamic_pointer_cast<ArrayIndexExpression>(exprChild)) {
                receiver = builder->CreateLoad(
                    llvm::PointerType::get(*module->getLlvmContext(), 0), receiver);
            }
        }

        // String built-in methods. String is a pointer alias, so the receiver value
        // is already the C-string ptr after the l-value coercion above. We detect a
        // String receiver via two paths: (a) static resolvedType says "String"; or
        // (b) the value is a plain `ptr` and the AST node isn't an array — covers
        // chained calls like `s.trim().isEmpty()` where the inner call doesn't yet
        // populate resolvedType.
        bool receiverIsString = false;
        if (receiver && receiverType && receiverType->getQName()
                && receiverType->getQName()->getTypeName() == "String") {
            receiverIsString = true;
        } else if (receiver && receiver->getType()->isPointerTy()) {
            auto childExpr = children.empty() ? nullptr
                : dynamic_pointer_cast<Expression>(children[0]);
            bool childIsArr = childExpr
                && dynamic_pointer_cast<CajetaArray>(childExpr->getResolvedType());
            if (!childIsArr) receiverIsString = true;
        }
        if (receiverIsString) {
            auto& llvmCtx = *module->getLlvmContext();
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
            auto callBool = [&](llvm::Function* fn, llvm::ArrayRef<llvm::Value*> args) -> llvm::Value* {
                llvm::Value* call = builder->CreateCall(fn, args);
                return builder->CreateICmpNE(call,
                    llvm::ConstantInt::get(call->getType(), 0));
            };
            auto load1 = [&]() -> llvm::Value* {
                return loadStringArg(module, parameters[0].expression);
            };
            // Coerce a numeric arg to i64 (signed-extended) for index-style parameters.
            auto loadIdx = [&](size_t i) -> llvm::Value* {
                llvm::Value* v = parameters[i].expression->generateCode(module);
                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                    v = builder->CreateLoad(a->getAllocatedType(), a);
                }
                if (v->getType()->isIntegerTy() && v->getType() != i64Ty) {
                    v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                }
                return v;
            };
            if (methodCallName == "size" || methodCallName == "length") {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_len");
                if (fn) return builder->CreateCall(fn, {receiver});
            }
            if (methodCallName == "isEmpty" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_isEmpty");
                if (fn) return callBool(fn, {receiver});
            }
            if (methodCallName == "equals" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_equals");
                if (fn) return callBool(fn, {receiver, load1()});
            }
            if (methodCallName == "charAt" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_charAt");
                if (fn) return builder->CreateCall(fn, {receiver, loadIdx(0)});
            }
            if (methodCallName == "indexOf" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_indexOf");
                if (fn) return builder->CreateCall(fn, {receiver, load1()});
            }
            if (methodCallName == "startsWith" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_startsWith");
                if (fn) return callBool(fn, {receiver, load1()});
            }
            if (methodCallName == "endsWith" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_endsWith");
                if (fn) return callBool(fn, {receiver, load1()});
            }
            if (methodCallName == "contains" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_contains");
                if (fn) return callBool(fn, {receiver, load1()});
            }
            if (methodCallName == "substring" && parameters.size() == 2) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_substring");
                if (fn) return builder->CreateCall(fn, {receiver, loadIdx(0), loadIdx(1)});
            }
            if (methodCallName == "toUpperCase" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_toUpperCase");
                if (fn) return builder->CreateCall(fn, {receiver});
            }
            if (methodCallName == "toLowerCase" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_toLowerCase");
                if (fn) return builder->CreateCall(fn, {receiver});
            }
            if (methodCallName == "trim" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_trim");
                if (fn) return builder->CreateCall(fn, {receiver});
            }
            if (methodCallName == "replace" && parameters.size() == 2) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_replace");
                if (fn) return builder->CreateCall(fn,
                    {receiver, load1(),
                     loadStringArg(module, parameters[1].expression)});
            }
        }

        // Structural accessors on arrays: size() reads the header's first field.
        if (receiver && methodCallName == "size") {
            if (auto arrayType = dynamic_pointer_cast<CajetaArray>(receiverType)) {
                llvm::Value* sizePtr = builder->CreateStructGEP(
                    arrayType->getLlvmType(), receiver, CajetaArray::SIZE_FIELD_INDEX);
                return builder->CreateLoad(
                    llvm::Type::getInt64Ty(*module->getLlvmContext()), sizePtr);
            }
        }

        // Resolve the target class either from the receiver (cross-object call) or from
        // the enclosing class on the structure stack (bare call).
        CajetaClassPtr targetClass;
        if (auto klass = dynamic_pointer_cast<CajetaClass>(receiverType)) {
            targetClass = klass;
        }
        if (!targetClass) {
            if (module->getStructureStack().empty()) {
                return nullptr;
            }
            targetClass = module->getStructureStack().back();
        }

        // Resolve `this`. For cross-object calls the receiver IS the `this`. For bare
        // calls we look it up from the active method's scope.
        llvm::Value* thisValue = receiver;
        if (!thisValue) {
            FieldPtr thisField = module->getScopeStack().peek()->getField("this");
            if (thisField) {
                llvm::AllocaInst* thisAlloca = thisField->getOrCreateAllocation();
                thisValue = builder->CreateLoad(thisAlloca->getAllocatedType(), thisAlloca);
            }
        }

        // Evaluate args, loading any l-values.
        vector<ParameterEntry> entries;
        for (auto& param : parameters) {
            llvm::Value* value = param.expression->generateCode(module);
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(value)) {
                value = builder->CreateLoad(a->getAllocatedType(), a);
            }
            entries.push_back(ParameterEntry(CajetaType::of(value), param.label, value));
        }

        return targetClass->invokeMethod(methodCallName, entries, /*isConstructor=*/false, thisValue);
    }


} // code
