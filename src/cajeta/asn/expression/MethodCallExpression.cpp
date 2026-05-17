//
// Created by James Klappenbach on 4/19/23.
//

#include "MethodCallExpression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaStruct.h"
#include "cajeta/type/CajetaView.h"
#include "cajeta/type/CajetaFunctionType.h"
#include "cajeta/method/Method.h"
#include "cajeta/error/Exception.h"
#include "Expression.h"
#include "DotExpression.h"
#include "Identifier.h"
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>

namespace cajeta {

    // Codegen dispatches three shapes:
    //   1. `arr.count()` on a CajetaArray receiver — structural accessor, loads the i64
    //      size field from the array header. Matches `Collection<T>.count()` so generic
    //      code over `Collection` works on T[] without special-casing.
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

        // ----- Indirect call through a function-typed local -----
        // `add(3, 4)` where `add` was declared as `(int32, int32) -> int32`.
        // The local's slot holds a `ptr` to a closure record
        // `{ ptr fn, ptr captures }` (L2 ABI). Load the closure, extract
        // both fields, and indirect-dispatch with captures prepended to the
        // user args. Matches when the call is bare (no receiver) AND a
        // scope lookup of methodCallName yields a function-typed field.
        // See cajeta-docs/Lambdas.md.
        if (children.empty() && !module->getScopeStack().isEmpty()) {
            auto scope = module->getScopeStack().peek();
            FieldPtr field = scope ? scope->getField(methodCallName) : nullptr;
            if (field) {
                auto fnType = dynamic_pointer_cast<CajetaFunctionType>(field->getType());
                if (fnType) {
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    // L3-3 closure layout: { ptr fn, ptr captures, ptr drop_fn }.
                    // The call site only reads fn (offset 0) and captures
                    // (offset 1); drop_fn (offset 2) is the runtime's
                    // concern at scope exit. Keeping the struct type
                    // consistent with the layout LambdaExpression emits
                    // so any future GEP arithmetic stays valid.
                    llvm::StructType* closureTy = llvm::StructType::get(
                        llvmCtx, {ptrTy, ptrTy, ptrTy});
                    llvm::AllocaInst* slot = field->getOrCreateAllocation();
                    llvm::Value* closurePtr = builder->CreateLoad(
                        ptrTy, slot, "closure_ptr");
                    llvm::Value* fnSlot = builder->CreateStructGEP(
                        closureTy, closurePtr, 0, "closure.fn");
                    llvm::Value* callee = builder->CreateLoad(
                        ptrTy, fnSlot, "fn_ptr");
                    llvm::Value* capSlot = builder->CreateStructGEP(
                        closureTy, closurePtr, 1, "closure.captures");
                    llvm::Value* captures = builder->CreateLoad(
                        ptrTy, capSlot, "captures_ptr");

                    vector<llvm::Value*> args;
                    args.push_back(captures);  // implicit first arg per L2 ABI
                    llvm::FunctionType* sig = fnType->getLlvmFunctionType();
                    for (size_t i = 0; i < parameters.size(); ++i) {
                        llvm::Value* v = parameters[i].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                            v = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        // Width-coerce to the function signature's expected
                        // param type (matches the coercion invokeMethod does
                        // for ordinary calls). Signature index is i + 1 to
                        // skip the captures slot.
                        size_t sigIdx = i + 1;
                        if (sig && sigIdx < sig->getNumParams() && v
                                && v->getType() != sig->getParamType(sigIdx)) {
                            llvm::Type* expected = sig->getParamType(sigIdx);
                            if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
                                v = builder->CreateIntCast(v, expected, /*isSigned=*/true);
                            } else if (expected->isFloatingPointTy()
                                    && v->getType()->isFloatingPointTy()) {
                                v = builder->CreateFPCast(v, expected);
                            }
                        }
                        args.push_back(v);
                    }
                    return builder->CreateCall(sig, callee, args);
                }
            }
        }

        // ----- Struct view construction: `MyStruct(byte[] bytes)` -----
        // Synthesizes the view: bounds-check (data.size() >= sizeof(struct))
        // then GEP into the array header's data region and return a typed
        // pointer. The struct's "instance" is just that pointer; field
        // accesses GEP off it.
        //
        // Matches when the call is bare (no receiver) AND the method name is
        // the canonical name of a registered CajetaStruct.
        if (children.empty() && parameters.size() == 1) {
            auto structType = dynamic_pointer_cast<CajetaView>(
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
                // S5: use getMinimumSize so multi-trailing var-size views
                // require their length-prefix bytes to be present too.
                uint64_t structBytes = structType->getMinimumSize();
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
                    // Error-model #202: runtime takes void*. IntToPtr the tag
                    // so the call type-checks against the new signature.
                    uint64_t tag = (uint64_t) 0xCA1E7A00 | (structBytes & 0xFF);
                    llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Value* tagPtr = builder->CreateIntToPtr(
                        llvm::ConstantInt::get(i64Ty, tag), ptrTy);
                    builder->CreateCall(throwFn, {tagPtr});
                }
                builder->CreateUnreachable();

                builder->SetInsertPoint(okBB);
                // GEP past the array header's i64 size field to reach data[0].
                llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                    i8Ty, bytesPtr,
                    llvm::ConstantInt::get(i64Ty, 8), "view_data_ptr");

                // S5.3 + S5b.3 — length-prefix validation sweep. Walks the
                // view's properties in declaration order, tracking a running
                // offset that grows by:
                //   - pre-first-var-size fixed: skip (already in fixedPrefixSize)
                //   - var-size field: read prefix, verify
                //         (offset + 4 + prefix) <= bufferBytes, advance
                //   - post-var fixed field: advance by its static size and
                //         verify offset+size <= bufferBytes
                //
                // An oversize length-prefix would let later accessors read
                // past the buffer end (a CVE class for wire-format parsers).
                // One pass at construction; per-access reads are bounds-
                // check-free.
                int varSizeCount = structType->getVariableSizeFieldCount();
                if (varSizeCount > 0) {
                    uint64_t fixedPrefixSize = structType->getFixedSize();
                    llvm::Value* offset = llvm::ConstantInt::get(
                        i64Ty, fixedPrefixSize);
                    const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
                    bool sawVar = false;
                    int diagIdx = 0;
                    for (auto& p : structType->getPropertyList()) {
                        bool isVar = CajetaAggregate::isVariableSize(p);
                        if (!sawVar && !isVar) {
                            // Pre-first-var fixed field: already in fixedPrefixSize.
                            continue;
                        }
                        if (isVar) sawVar = true;
                        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(
                            llvmCtx, "vlen_fail", parentFn);
                        llvm::BasicBlock* okBB2 = llvm::BasicBlock::Create(
                            llvmCtx, "vlen_ok", parentFn);
                        llvm::Value* afterField = nullptr;
                        if (isVar) {
                            // Read i32 prefix at (dataPtr + offset), then advance.
                            llvm::Value* prefixPtr = builder->CreateInBoundsGEP(
                                i8Ty, dataPtr, offset, "vlen_prefix_ptr");
                            llvm::Value* prefix32 = builder->CreateLoad(
                                llvm::Type::getInt32Ty(llvmCtx), prefixPtr,
                                "vlen_prefix");
                            llvm::Value* prefix64 = builder->CreateIntCast(
                                prefix32, i64Ty, /*isSigned=*/true);
                            llvm::Value* fourPlus = builder->CreateAdd(
                                prefix64, llvm::ConstantInt::get(i64Ty, 4),
                                "vlen_after_prefix");
                            afterField = builder->CreateAdd(
                                offset, fourPlus, "vlen_after_field");
                        } else {
                            // Post-var fixed field: advance by static size.
                            uint64_t sz = dl.getTypeAllocSize(p->getType()->getLlvmType());
                            afterField = builder->CreateAdd(
                                offset, llvm::ConstantInt::get(i64Ty, sz),
                                "vlen_after_postvar_fixed");
                        }
                        llvm::Value* vlenOk = builder->CreateICmpULE(
                            afterField, haveBytes, "vlen_ok");
                        builder->CreateCondBr(vlenOk, okBB2, failBB);
                        builder->SetInsertPoint(failBB);
                        if (llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw")) {
                            uint64_t tag = (uint64_t) 0xCA1E7A00 | (uint64_t)(diagIdx & 0xFF);
                            llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                            llvm::Value* tagPtr = builder->CreateIntToPtr(
                                llvm::ConstantInt::get(i64Ty, tag), ptrTy);
                            builder->CreateCall(throwFn, {tagPtr});
                        }
                        builder->CreateUnreachable();
                        builder->SetInsertPoint(okBB2);
                        offset = afterField;
                        diagIdx++;
                    }
                }

                return dataPtr;
            }
        }

        // ----- Bare class-construction syntax rejected (UnifiedClasses.md P1b) -----
        // `MyClass(args)` without an explicit `heap` / `stack` / `new`
        // prefix is ambiguous (parses as a methodCall) and now rejected in
        // v2. Catches the case where the "method name" resolves to a class
        // type. Views keep their legacy `MyView(bytes)` form (handled
        // above via the view-construction path); interfaces aren't
        // constructible at all and would fail downstream anyway.
        if (children.empty()) {
            auto resolvedType = CajetaType::of(methodCallName);
            auto classType = std::dynamic_pointer_cast<CajetaClass>(resolvedType);
            auto viewType  = std::dynamic_pointer_cast<CajetaView>(resolvedType);
            if (classType && !classType->isInterface() && !viewType) {
                char buf[640];
                snprintf(buf, sizeof(buf),
                    "class '%s' cannot be constructed via bare `%s(...)` "
                    "syntax in the unified-class model. Use `heap %s(...)` "
                    "for heap allocation or `stack %s(...)` for stack "
                    "allocation (`stack` lands fully in Phase 2). The "
                    "`new` keyword continues to work during the deprecation "
                    "cycle.",
                    methodCallName.c_str(), methodCallName.c_str(),
                    methodCallName.c_str(), methodCallName.c_str());
                throw Exception(buf, "CAJETA_ERROR_BARE_CLASS_CONSTRUCTION");
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
                    // Use the shared l-value-to-r-value coercion: an
                    // arg expression might be a local alloca, an array
                    // GEP, a struct/class field GEP (DotExpression), or
                    // a class-field implicit-this GEP from
                    // IdentifierExpression. All of those need a load
                    // before the value flows into the runtime helper.
                    auto& p = parameters[i].expression;
                    llvm::Value* v = p->generateCode(module);
                    auto ast = dynamic_pointer_cast<Expression>(p);
                    if (ast && !ast->getResolvedType()) {
                        ast->resolveTypes(module);
                    }
                    return loadIfLValue(module, v, ast);
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
                // @PreDestroy follow-up: explicit runtime trigger for
                // the registered atexit handlers. AOT binaries call
                // this from main() before returning; tests fire it
                // mid-test to observe @PreDestroy side effects. The
                // runtime clears its registry after handlers fire so
                // a second call is a no-op (and safe).
                if (ns == "Cajeta" && methodCallName == "runAtExit" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_run_atexit_handlers");
                    return builder->CreateCall(fn, {});
                }
                // Threading sync primitives — Lock. These are low-level
                // intrinsics; the user-facing `Lock` class (with an
                // `acquire()` that returns a RAII guard) will wrap them
                // once user-defined-drop-on-class machinery lands. For
                // now Cajeta source can use them directly. See
                // ThreadModel.md § Synchronization primitives.
                if (ns == "Cajeta" && methodCallName == "lockNew" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_new");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "lockAcquire" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_acquire");
                    llvm::Value* h = loadValue(0);
                    return builder->CreateCall(fn, {h});
                }
                if (ns == "Cajeta" && methodCallName == "lockRelease" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_release");
                    llvm::Value* h = loadValue(0);
                    return builder->CreateCall(fn, {h});
                }
                if (ns == "Cajeta" && methodCallName == "lockTryAcquire" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_try_acquire");
                    llvm::Value* h = loadValue(0);
                    return builder->CreateCall(fn, {h});
                }
                if (ns == "Cajeta" && methodCallName == "lockDestroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_destroy");
                    llvm::Value* h = loadValue(0);
                    return builder->CreateCall(fn, {h});
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
            // the referenced object (CajetaArray inner header or class instance). A
            // class-name receiver (`Bar.staticMethod()`) carries a null IR value with
            // a null resolvedType (IdentifierExpression intentionally doesn't pin
            // class names — see Identifier.cpp). Skip the coercion when the IR
            // value is null; the class-name fallback below sets receiverType so the
            // static-dispatch path picks up targetClass.
            if (receiver) {
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(receiver)) {
                    receiver = builder->CreateLoad(a->getAllocatedType(), a);
                } else if (dynamic_pointer_cast<ArrayIndexExpression>(exprChild)) {
                    receiver = builder->CreateLoad(
                        llvm::PointerType::get(*module->getLlvmContext(), 0), receiver);
                }
            }
            // Class-name receiver fallback. `Bar.staticMethod()` parses as
            // expression-DOT-methodCall; the LHS IdentifierExpression
            // doesn't resolve to a local or field, so generateCode returns
            // null and resolveTypes leaves resolvedType null. Look up the
            // bare identifier in canonicalMap (which is keyed by both
            // short typeName and full canonical) — a match means the
            // receiver named a class and we can route through static
            // dispatch.
            if (!receiver && !receiverType) {
                if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(exprChild)) {
                    auto& cmap = CajetaType::getCanonicalMap();
                    auto it = cmap.find(idExpr->getTextValue());
                    if (it != cmap.end()
                            && dynamic_pointer_cast<CajetaClass>(it->second)) {
                        receiverType = it->second;
                    }
                }
            }
        }

        // Primitive-receiver intrinsic: `<int32>.hash()` and friends
        // lower directly to the matching __cajeta_hash_X runtime
        // helper, with sext/zext coercion for narrow ints / boolean
        // (i1) so the call's argument type matches the helper's C
        // ABI. Lets HashMap<int32, V> (and other primitive-keyed
        // maps) work without boxing — the template body's
        // `key.hash()` resolves to int32 after instantiation and
        // hits this branch instead of trying to dispatch a method
        // that primitives don't have.
        //
        // Narrow signed ints sign-extend; narrow unsigned ints zero-
        // extend. Matches the coercion rules in
        // SynthesizedHashMethod so a `int32 x` hashed via @AutoHash
        // and a `int32 x` hashed via `x.hash()` produce the same
        // value for the same x.
        if (receiver && receiverType
                && methodCallName == "hash"
                && parameters.empty()
                && (receiverType->getTypeFlags() & PRIMITIVE_FLAG)
                && !dynamic_pointer_cast<CajetaClass>(receiverType)) {
            auto& llvmCtx = *module->getLlvmContext();
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
            llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);

            const char* symbol = nullptr;
            llvm::Type* argTy = nullptr;
            llvm::Value* arg = receiver;

            switch (receiverType->getTypeFlags() & TYPE_ID_MASK) {
                case BOOLEAN_ID:
                    symbol = "__cajeta_hash_boolean";
                    arg = builder->CreateZExt(receiver, i8Ty);
                    argTy = i8Ty;
                    break;
                case INT8_ID:
                    symbol = "__cajeta_hash_int32";
                    arg = builder->CreateSExt(receiver, i32Ty);
                    argTy = i32Ty;
                    break;
                case UINT8_ID:
                    symbol = "__cajeta_hash_int32";
                    arg = builder->CreateZExt(receiver, i32Ty);
                    argTy = i32Ty;
                    break;
                case INT16_ID:
                    symbol = "__cajeta_hash_int32";
                    arg = builder->CreateSExt(receiver, i32Ty);
                    argTy = i32Ty;
                    break;
                case UINT16_ID:
                    symbol = "__cajeta_hash_int32";
                    arg = builder->CreateZExt(receiver, i32Ty);
                    argTy = i32Ty;
                    break;
                case INT32_ID:
                case UINT32_ID:
                    symbol = "__cajeta_hash_int32";
                    argTy = i32Ty;
                    break;
                case INT64_ID:
                case UINT64_ID:
                    symbol = "__cajeta_hash_int64";
                    argTy = i64Ty;
                    break;
                case FLOAT32_ID:
                    symbol = "__cajeta_hash_float32";
                    argTy = llvm::Type::getFloatTy(llvmCtx);
                    break;
                case FLOAT64_ID:
                    symbol = "__cajeta_hash_float64";
                    argTy = llvm::Type::getDoubleTy(llvmCtx);
                    break;
                default:
                    // Extended-precision (fp4/6/8/16/128), int128/
                    // uint128, bare `pointer` — no specialized hash
                    // helper today. Fall through to the regular
                    // dispatch path; it'll fail loudly because
                    // primitives don't carry hash() as a method.
                    break;
            }
            if (symbol) {
                llvm::FunctionType* fnTy = llvm::FunctionType::get(
                    i64Ty, { argTy }, false);
                llvm::Module* lmod = module->getLlvmModule();
                llvm::Function* fn = lmod->getFunction(symbol);
                if (!fn) {
                    fn = llvm::Function::Create(
                        fnTy, llvm::Function::ExternalLinkage,
                        symbol, lmod);
                }
                return builder->CreateCall(fn, { arg });
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
        } else if (receiver && receiver->getType()->isPointerTy()
                && !dynamic_pointer_cast<CajetaClass>(receiverType)) {
            // Fallback for chained calls where the inner call hasn't
            // populated its resolvedType (e.g. `s.trim().isEmpty()`).
            // Only fires when receiverType ISN'T a known CajetaClass —
            // otherwise `someClass.size()` on a user class would
            // hijack into __cajeta_str_len instead of dispatching to
            // the class's own size() method. CajetaArray (which
            // inherits CajetaClass) also blocks this path; the array's
            // size() routes through the dedicated structural-accessor
            // branch below.
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

        // Structural accessor on arrays. `count()` is the only name —
        // matches the Collection interface so generic code that operates
        // on Collection<T> works on T[] without special-casing. Reads
        // the header's first field (i64 count).
        if (receiver && methodCallName == "count") {
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

        // Evaluate args, loading any l-values. Each entry carries the
        // expression's resolved type when known (fall back to of(value) for
        // primitive values that have a clean LLVM type).
        vector<ParameterEntry> entries;
        for (auto& param : parameters) {
            if (!param.expression->getResolvedType()) {
                param.expression->resolveTypes(module);
            }
            llvm::Value* value = param.expression->generateCode(module);
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(value)) {
                value = builder->CreateLoad(a->getAllocatedType(), a);
            }
            CajetaTypePtr et = param.expression->getResolvedType();
            if (!et) et = CajetaType::of(value);
            entries.push_back(ParameterEntry(et, param.label, value));
        }

        // Varargs (`T... args`): if a same-named method on the target class is
        // marked varargs and the call site is supplying enough fixed args
        // plus zero-or-more trailing values, pack the trailing values into a
        // fresh T[] and replace them with the single array argument before
        // dispatch. The check looks for a name match (not full signature)
        // because varargs is the only case where the call's arg count is
        // expected to differ from a target method's parameter count.
        MethodPtr varargsTarget;
        for (auto& mEntry : targetClass->getMethods()) {
            auto& m = mEntry.second;
            if (m->isVarargs() && m->getName() == methodCallName) {
                varargsTarget = m;
                break;
            }
        }
        if (varargsTarget) {
            // parameterList still has the implicit `this` slot prepended for
            // instance methods, so the fixed arg count (what the call must
            // supply before the vararg pack) is total - 1 (the vararg
            // T[] slot) - (1 if non-static, else 0).
            auto paramList = varargsTarget->getParameterList();
            bool isStatic = varargsTarget->getModifiers().find(STATIC)
                != varargsTarget->getModifiers().end();
            int totalParams = (int) paramList.size();
            int fixedArgs = totalParams - 1 - (isStatic ? 0 : 1);
            if (fixedArgs < 0) fixedArgs = 0;
            if ((int) entries.size() >= fixedArgs) {
                // Determine T from the varargs param (the last slot).
                auto varParam = paramList.empty() ? nullptr : paramList.back();
                auto arrType = varParam
                    ? dynamic_pointer_cast<CajetaArray>(varParam->getType())
                    : nullptr;
                if (arrType) {
                    auto& llvmCtx = *module->getLlvmContext();
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    const llvm::DataLayout& dl =
                        module->getLlvmModule()->getDataLayout();
                    llvm::Type* headerTy = arrType->getLlvmType();
                    llvm::Type* elemTy = arrType->getElementLlvmType(&llvmCtx);
                    int trailing = (int) entries.size() - fixedArgs;
                    if (auto allocFn = module->getRuntimeFunction(
                            "__cajeta_new_array_header")) {
                        llvm::Value* hdrPtr = builder->CreateCall(allocFn, {
                            llvm::ConstantInt::get(i64Ty, dl.getTypeAllocSize(headerTy)),
                            llvm::ConstantInt::get(i64Ty, dl.getTypeAllocSize(elemTy)),
                            llvm::ConstantInt::get(i64Ty, trailing),
                        });
                        for (int i = 0; i < trailing; ++i) {
                            llvm::Value* v = entries[fixedArgs + i].value;
                            if (v && v->getType() != elemTy && elemTy->isIntegerTy()
                                    && v->getType()->isIntegerTy()) {
                                v = builder->CreateIntCast(v, elemTy, /*isSigned=*/true);
                            }
                            vector<llvm::Value*> gepIndices = {
                                llvm::ConstantInt::get(i64Ty, 0),
                                llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
                                llvm::ConstantInt::get(i64Ty, i),
                            };
                            llvm::Value* slot = builder->CreateGEP(headerTy, hdrPtr, gepIndices);
                            builder->CreateStore(v, slot);
                        }
                        // Trim entries to fixed args + the new array entry.
                        entries.erase(entries.begin() + fixedArgs, entries.end());
                        entries.push_back(ParameterEntry(arrType, "", hdrPtr));
                    }
                }
            }
        }

        // Default parameter values: if a same-named method on the class has
        // defaults for its trailing parameters and the call supplies fewer
        // args than the method expects, fill the missing slots from the
        // method's default expressions. Match by name + arity-range
        // (required..total) — the first method whose [required, total]
        // window contains the call's arg count wins.
        if (!targetClass->getMethods().empty()) {
            for (auto& mEntry : targetClass->getMethods()) {
                auto& m = mEntry.second;
                if (m->getName() != methodCallName) continue;
                if (m->isVarargs()) continue;
                bool isStatic = m->getModifiers().find(STATIC)
                    != m->getModifiers().end();
                auto pl = m->getParameterList();
                int thisShift = isStatic ? 0 : 1;
                int userParams = (int) pl.size() - thisShift;
                if ((int) entries.size() >= userParams) continue;
                int required = 0;
                for (int i = thisShift; i < (int) pl.size(); ++i) {
                    if (pl[i]->getDefaultValue()) break;
                    required++;
                }
                if ((int) entries.size() < required) continue;
                // Eligible: emit each missing default expression.
                for (int i = thisShift + (int) entries.size();
                     i < (int) pl.size(); ++i) {
                    auto defExpr = pl[i]->getDefaultValue();
                    if (!defExpr) break;
                    if (!defExpr->getResolvedType()) {
                        defExpr->resolveTypes(module);
                    }
                    llvm::Value* dv = defExpr->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(dv)) {
                        dv = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    CajetaTypePtr dt = defExpr->getResolvedType();
                    if (!dt) dt = CajetaType::of(dv);
                    entries.push_back(ParameterEntry(dt, "", dv));
                }
                break;
            }
        }

        // Uncaught-throws lint (rule ID `uncaught-throws`). If the resolved
        // target declares `throws X, Y`, walk that list and warn for each
        // entry that the enclosing method doesn't itself declare AND no
        // enclosing try/catch covers. Advisory only — no compile error,
        // matching ErrorModel.md's "throws is documentation" position.
        // Suppressible per-method via `@SuppressLint("uncaught-throws")`
        // — see LintRules.md for the catalog.
        bool floatingParamsLint = true;
        for (auto& p : entries) if (p.label.empty()) { floatingParamsLint = false; break; }
        vector<ParameterEntry> entriesCopy = entries;
        MethodPtr targetMethod = targetClass->resolveMethod(
            methodCallName, entriesCopy, /*isConstructor=*/false, floatingParamsLint);
        if (targetMethod) {
            auto& throwsList = targetMethod->getThrowsList();
            if (!throwsList.empty()) {
                auto currentMethod = module->getCurrentMethod();
                if (currentMethod
                        && !currentMethod->isLintSuppressed("uncaught-throws")) {
                    auto& currentThrows = currentMethod->getThrowsList();
                    // isCaughtBy: the thrown name is caught by catchType when
                    // they share a canonical name (or short name, since the
                    // throwsList carries unqualified-by-default qNames just
                    // like the existing declaration walk above), OR when
                    // catchType is an ancestor of the thrown class. The
                    // ancestor walk needs a resolved CajetaClass; if the
                    // throwsList name doesn't resolve (e.g. unknown type),
                    // we fall back to the name-only check.
                    auto isCaughtBy = [&](const QualifiedNamePtr& thrownQ,
                                          const CajetaTypePtr& catchType) -> bool {
                        if (!thrownQ || !catchType) return false;
                        const string& catchCanonical = catchType->toCanonical();
                        if (thrownQ->toCanonical() == catchCanonical
                                || thrownQ->getTypeName() == catchCanonical) {
                            return true;
                        }
                        // Resolve thrown to a class so we can walk its
                        // supertypes. Mirrors the resolution used in
                        // TryStatement::generateCode when parsing catch
                        // types (CajetaType::of by canonical, then by
                        // short name).
                        auto thrownType = CajetaType::of(thrownQ);
                        if (!thrownType) {
                            thrownType = CajetaType::of(thrownQ->getTypeName(), "");
                        }
                        auto thrownClass = dynamic_pointer_cast<CajetaClass>(thrownType);
                        if (!thrownClass) return false;
                        // Walk includes self — the name comparison at the top
                        // catches throwsClause-name vs catchType-canonical
                        // matches, but those canonicals can diverge when the
                        // throws-clause parser defaults to package `code`
                        // while the resolved class lives in the user's
                        // declared package (e.g. `test.IOException`).
                        // Walking from the resolved thrown class lets the
                        // exact-match case route through the supertype path
                        // using the same canonical string the catch type was
                        // resolved to.
                        std::function<bool(const CajetaClassPtr&)> walk =
                            [&](const CajetaClassPtr& cls) -> bool {
                                if (cls->toCanonical() == catchCanonical) return true;
                                for (auto& parent : cls->getSuperClasses()) {
                                    if (walk(parent)) return true;
                                }
                                return false;
                            };
                        return walk(thrownClass);
                    };
                    auto& tryCatchStack = module->getTryCatchStack();
                    for (auto& thrownType : throwsList) {
                        bool declared = false;
                        for (auto& declaredType : currentThrows) {
                            if (thrownType->toCanonical() == declaredType->toCanonical()) {
                                declared = true;
                                break;
                            }
                        }
                        if (declared) continue;
                        // Coverage check (#209): any enclosing try whose catch
                        // arms catch thrownType suppresses the warning. Walk
                        // the stack innermost-out, but the order doesn't
                        // actually matter — coverage anywhere suffices.
                        bool covered = false;
                        for (auto& frame : tryCatchStack) {
                            for (auto& catchType : frame) {
                                if (isCaughtBy(thrownType, catchType)) {
                                    covered = true;
                                    break;
                                }
                            }
                            if (covered) break;
                        }
                        if (!covered) {
                            std::cerr << "warning: [uncaught-throws] call to "
                                << methodCallName
                                << " can throw " << thrownType->toCanonical()
                                << " but enclosing " << currentMethod->getName()
                                << " neither catches nor declares it"
                                << std::endl;
                        }
                    }
                }
            }
        }

        llvm::Value* callResult = targetClass->invokeMethod(methodCallName, entries,
            /*isConstructor=*/false, thisValue, /*callerModule=*/module);

        // S6.7 — repackage a struct-returning call. The callee returns the
        // struct VALUE (per Method::generatePrototype, CajetaStruct returns
        // travel by value to dodge the dangling-pointer-on-stack-death that
        // a `ptr` return convention would create). Downstream consumers
        // (HeapField slots, parameter pass-by-pointer) all expect a body
        // pointer though, so wrap the result in a fresh caller-side alloca
        // + store. Skips void / non-CajetaStruct returns — those return
        // values that already fit the existing flow.
        if (callResult && targetClass) {
            for (auto& mEntry : targetClass->getMethods()) {
                auto& m = mEntry.second;
                if (!m || m->getName() != methodCallName) continue;
                auto rt = m->getReturnType();
                if (auto structRet = dynamic_pointer_cast<CajetaStruct>(rt)) {
                    if (llvm::Type* bodyTy = structRet->getLlvmType()) {
                        if (callResult->getType() == bodyTy) {
                            llvm::Value* bodyAlloca =
                                builder->CreateAlloca(bodyTy);
                            builder->CreateStore(callResult, bodyAlloca);
                            return bodyAlloca;
                        }
                    }
                    break;
                }
                // S9.5.5 — interface returns also travel by value
                // (per Method::generatePrototype's S9.5.5 carve-out).
                // Repackage into a fresh caller-side body alloca so
                // downstream code (HeapField slot store, dispatch) sees
                // a body pointer.
                if (auto retClass = dynamic_pointer_cast<CajetaClass>(rt)) {
                    if (retClass->isInterface()) {
                        if (llvm::Type* bodyTy = retClass->getLlvmType()) {
                            if (callResult->getType() == bodyTy) {
                                llvm::Value* bodyAlloca =
                                    builder->CreateAlloca(bodyTy);
                                builder->CreateStore(callResult, bodyAlloca);
                                return bodyAlloca;
                            }
                        }
                        break;
                    }
                }
                break;
            }
        }
        return callResult;
    }


} // code
