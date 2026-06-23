//
// Created by James Klappenbach on 4/19/23.
//

#include "MethodCallExpression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaVector.h"
#include "cajeta/type/VectorOps.h"
#include "cajeta/type/CajetaMatrix.h"
#include "cajeta/type/MatrixOps.h"
#include "cajeta/type/CajetaQuaternion.h"
#include "cajeta/type/QuaternionOps.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaConstantType.h"
#include "cajeta/type/CajetaTask.h"
#include "cajeta/type/CajetaView.h"
#include "cajeta/type/CajetaFunctionType.h"
#include "cajeta/method/Method.h"
#include "cajeta/error/Diagnostics.h"
#include "cajeta/error/Exception.h"
#include "cajeta/util/MemoryManager.h"
#include "Expression.h"
#include "DotExpression.h"
#include "Identifier.h"
#include "LiteralExpression.h"
#include "NewExpression.h"
#include "cajeta/type/Scope.h"
#include "cajeta/field/Field.h"
#include "cajeta/field/ParameterField.h"
#include "cajeta/field/BoundClosureField.h"
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>

namespace cajeta {

    // Wrap a malloc'd null-terminated C-string into a fresh
    // `cajeta.lang.String` instance the user code can dispatch
    // class methods against. The legacy runtime helpers
    // (`__cajeta_i64_to_str`, `__cajeta_f64_to_str`,
    // `__cajeta_bool_to_str`, `__cajeta_str_fromChar`,
    // `__cajeta_str_concat`, …) all return `char*` and were the
    // entire String surface before the class String landed; after
    // Phase 2b-β user code expects class instances at the same call
    // sites. This helper bridges: takes the malloc'd cstr, copies
    // its bytes into a CajetaArray header so `s.bytes[i]` works
    // through the int8[] field shape, allocates a class String
    // header with the right vtable, frees the intermediate cstr,
    // and returns the class String pointer. Returns the raw cstr
    // unchanged when the class String type hasn't been loaded yet
    // (runtime bring-up bootstrap window).
    llvm::Value* wrapCStringIntoClassString(
            CajetaModulePtr module,
            llvm::Value* cstr,
            const char* namePrefix,
            bool freeAfterWrap) {
        auto& llvmCtx = *module->getLlvmContext();
        auto* builder = module->getBuilder();
        llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);

        CajetaTypePtr stringTy = CajetaType::of("String");
        auto stringKlass = std::dynamic_pointer_cast<CajetaClass>(stringTy);
        if (!stringKlass || !stringKlass->getLlvmType()
                || !llvm::isa<llvm::StructType>(stringKlass->getLlvmType())) {
            return cstr;  // bootstrap fallback
        }
        auto* stringStructTy =
            llvm::cast<llvm::StructType>(stringKlass->getLlvmType());

        std::string pfx = namePrefix ? namePrefix : "str";
        llvm::Function* strlenFn = module->getRuntimeFunction("__cajeta_str_len");
        llvm::Value* lenI64 = builder->CreateCall(
            strlenFn, {cstr}, pfx + ".len");
        llvm::Value* lenI32 = builder->CreateIntCast(
            lenI64, i32Ty, /*isSigned=*/true, pfx + ".len32");

        // CajetaArray header sized 8 + len + 1 (count word + bytes +
        // null terminator).
        llvm::Value* arrSize = builder->CreateAdd(lenI64,
            llvm::ConstantInt::get(i64Ty, 9), pfx + ".arr_size");
        llvm::FunctionType* allocTy = llvm::FunctionType::get(
            ptrTy, {i64Ty}, false);
        llvm::FunctionCallee allocFn =
            module->getLlvmModule()->getOrInsertFunction(
                "__cajeta_alloc", allocTy);
        llvm::Value* arrPtr = builder->CreateCall(
            allocFn, {arrSize}, pfx + ".arr_alloc");
        builder->CreateMemSet(arrPtr,
            llvm::ConstantInt::get(i8Ty, 0),
            arrSize, llvm::MaybeAlign(8));
        builder->CreateStore(lenI64, arrPtr);
        llvm::Value* dataPtr = builder->CreateInBoundsGEP(
            i8Ty, arrPtr,
            llvm::ConstantInt::get(i64Ty, 8),
            pfx + ".arr_data");
        builder->CreateMemCpy(dataPtr, llvm::MaybeAlign(1),
            cstr, llvm::MaybeAlign(1), lenI64);

        const llvm::DataLayout& dl =
            module->getLlvmModule()->getDataLayout();
        llvm::Constant* sSize = llvm::ConstantInt::get(
            i64Ty, dl.getTypeAllocSize(stringStructTy));
        llvm::Value* sPtr = MemoryManager::createMallocInstruction(
            module, sSize, builder->GetInsertBlock());

        llvm::Constant* vtableRef = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptrTy));
        if (auto* vt = stringKlass->getVirtualTableGlobal()) {
            vtableRef = CajetaModule::ensureGlobalInModule(
                module->getLlvmModule(), vt);
        }
        builder->CreateStore(vtableRef,
            builder->CreateStructGEP(stringStructTy, sPtr, 0,
                pfx + ".s_vtable"));
        builder->CreateStore(arrPtr,
            builder->CreateStructGEP(stringStructTy, sPtr, 1,
                pfx + ".s_bytes"));
        builder->CreateStore(lenI32,
            builder->CreateStructGEP(stringStructTy, sPtr, 2,
                pfx + ".s_byteLength"));
        builder->CreateStore(llvm::ConstantInt::get(i32Ty, 0),
            builder->CreateStructGEP(stringStructTy, sPtr, 3,
                pfx + ".s_mode"));
        builder->CreateStore(llvm::ConstantInt::get(i32Ty, -1),
            builder->CreateStructGEP(stringStructTy, sPtr, 4,
                pfx + ".s_cachedCpLength"));

        // Free the intermediate cstr — we copied its bytes into the
        // CajetaArray header, so the class String owns its own
        // buffer. Caller passes `freeAfterWrap=false` when the cstr
        // is a `.rodata` static (e.g. `__cajeta_bool_to_str` returns
        // a const char* to a string literal — calling free() on it
        // would crash). The malloc'd-cstr helpers (i64_to_str,
        // f64_to_str, str_concat, …) all want the free.
        if (freeAfterWrap) {
            if (llvm::Function* freeFn =
                    module->getRuntimeFunction("__cajeta_free")) {
                builder->CreateCall(freeFn, {cstr});
            }
        }
        return sPtr;
    }

    // Pass-by-pointer ABI bridge for closure-call arguments. Class / interface /
    // array params cross the call boundary as `ptr` (see
    // CajetaFunctionType::toCallingConvType and Method::generatePrototype), but a
    // closure arg can be materialized as the aggregate VALUE instead — e.g. an
    // interface fat-pointer struct loaded from an array or stream slot. Spilling
    // that value to an entry-block stack slot and passing its address makes the
    // call match the closure's `ptr` parameter. Without this, an interface-typed
    // closure argument fails LLVM verify ("Call parameter type does not match
    // function signature"); repro: a stream / forEach consumer over an
    // ArrayList<SomeInterface>. Unlike the int/float width coercions next to it,
    // this case is a value→pointer ABI fix, not a representation change.
    static llvm::Value* spillAggregateForByPointerArg(CajetaModulePtr module,
                                                      llvm::Value* v) {
        auto* builder = module->getBuilder();
        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(
            &curFn->getEntryBlock(), curFn->getEntryBlock().begin());
        llvm::Value* slot = entryBuilder.CreateAlloca(
            v->getType(), nullptr, "byptr_arg");
        builder->CreateStore(v, slot);
        return slot;
    }

    // Indirect call through a closure value — shared by the bare-identifier
    // form `op(args)` (MethodCallExpression) and the postfix expression/indexed
    // form `arr[i](args)` (CallExpression). `closurePtr` is a `ptr` to the
    // closure record `{ ptr fn, ptr captures, ptr drop_fn }` (L3-3 ABI); the
    // call reads fn (offset 0) + captures (offset 1) and dispatches with
    // captures prepended to the user args. Declared in MethodCallExpression.h.
    llvm::Value* emitClosureCall(CajetaModulePtr module,
                                 llvm::Value* closurePtr,
                                 const std::shared_ptr<CajetaFunctionType>& fnType,
                                 const vector<MethodCallParameter>& args,
                                 CajetaTypePtr& outResolvedType,
                                 llvm::Function* directFn) {
        auto& llvmCtx = *module->getLlvmContext();
        auto* builder = module->getBuilder();
        llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
        // L3-3 closure layout: { ptr fn, ptr captures, ptr drop_fn }. The call
        // site only reads fn + captures; drop_fn is the runtime's concern at
        // scope exit. Keeping the struct shape consistent with the layout
        // LambdaExpression / MethodReferenceExpression emit so GEPs stay valid.
        llvm::StructType* closureTy = llvm::StructType::get(
            llvmCtx, {ptrTy, ptrTy, ptrTy});
        // Specialization fast path (cajeta-ir Unit 4): a statically-known target
        // calls direct, with captures folded to null (the non-capturing lambda
        // ignores them). Otherwise load fn + captures from the closure record.
        llvm::Value* callee;
        llvm::Value* captures;
        if (directFn) {
            callee = directFn;
            captures = llvm::ConstantPointerNull::get(ptrTy);
        } else {
            llvm::Value* fnSlot = builder->CreateStructGEP(
                closureTy, closurePtr, 0, "closure.fn");
            callee = builder->CreateLoad(ptrTy, fnSlot, "fn_ptr");
            llvm::Value* capSlot = builder->CreateStructGEP(
                closureTy, closurePtr, 1, "closure.captures");
            captures = builder->CreateLoad(ptrTy, capSlot, "captures_ptr");
        }

        // M5(b) — sret form: caller allocates the result slot in its own frame
        // and threads it as the closure's hidden arg 0. The call returns void;
        // the call's value is the slot pointer (chained into like a class
        // instance pointer).
        llvm::Value* sretSlot = nullptr;
        auto retClass = dynamic_pointer_cast<CajetaClass>(fnType->getReturnType());
        if (fnType->usesSret() && retClass) {
            llvm::Function* curFn = builder->GetInsertBlock()->getParent();
            llvm::IRBuilder<> entryBuilder(
                &curFn->getEntryBlock(), curFn->getEntryBlock().begin());
            sretSlot = entryBuilder.CreateAlloca(
                retClass->getLlvmType(), nullptr, "fn_sret");
        }
        vector<llvm::Value*> callArgs;
        if (sretSlot) callArgs.push_back(sretSlot);
        callArgs.push_back(captures);  // implicit captures arg per L2 ABI
        size_t baseIdx = sretSlot ? 2 : 1;
        llvm::FunctionType* sig = fnType->getLlvmFunctionType();
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i].expression && !args[i].expression->getResolvedType()) {
                args[i].expression->resolveTypes(module);
            }
            llvm::Value* v = args[i].expression->generateCode(module);
            // l-value coercion: handles ArrayIndex / Dot GEPs uniformly so a
            // primitive element / field load is passed by value, not as the
            // slot pointer (JIT verify rejects a ptr where a scalar is wanted).
            auto exprAst = dynamic_pointer_cast<Expression>(args[i].expression);
            v = loadIfLValue(module, v, exprAst);
            // Width-coerce to the signature's expected param type (matches the
            // coercion invokeMethod does for ordinary calls). Signature index
            // is baseIdx + i (sret-shifted when applicable).
            size_t sigIdx = baseIdx + i;
            if (sig && sigIdx < sig->getNumParams() && v
                    && v->getType() != sig->getParamType(sigIdx)) {
                llvm::Type* expected = sig->getParamType(sigIdx);
                if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
                    v = builder->CreateIntCast(v, expected, /*isSigned=*/true);
                } else if (expected->isFloatingPointTy()
                        && v->getType()->isFloatingPointTy()) {
                    v = builder->CreateFPCast(v, expected);
                } else if (expected->isPointerTy() && !v->getType()->isPointerTy()) {
                    v = spillAggregateForByPointerArg(module, v);
                }
            }
            callArgs.push_back(v);
        }
        llvm::CallInst* call = builder->CreateCall(sig, callee, callArgs);
        if (sretSlot && retClass) {
            call->addParamAttr(0, llvm::Attribute::get(
                llvmCtx, llvm::Attribute::StructRet, retClass->getLlvmType()));
            // Pin resolvedType so chained `.field` / `.method()` see the
            // value-typed result the sret slot holds.
            if (fnType->getReturnType()) outResolvedType = fnType->getReturnType();
            return sretSlot;
        }
        return call;
    }

    // methodCall: `identifier ('<' typeList '>')? '(' parameterList? ')'`
    //           | `THIS '(' parameterList? ')'`
    //           | `SUPER '(' parameterList? ')'`
    //
    // The optional `<typeList>` between name and '(' is Form C explicit
    // call-site type args for method-templated callees (see
    // docs/specification/lang/MethodLevelTemplate.md). Inference is the
    // common case; explicit args are required only when inference
    // can't bind every type parameter (e.g. T appears only in the
    // return type).
    MethodCallExpression::MethodCallExpression(
        CajetaParser::MethodCallContext* ctx,
        antlr4::Token* token) : Expression(token) {
        if (ctx->SUPER()) {
            superCtorCall = true;
            methodCallName = "super";
        } else if (ctx->identifier()) {
            methodCallName = ctx->identifier()->getText();
        } else {
            // THIS '(' ... ')' form — explicit this(args) ctor delegation;
            // not implemented today. Mark with a placeholder name so codegen
            // can recognize-and-reject (rather than null-deref).
            methodCallName = "this";
        }
        if (auto* paramList = ctx->parameterList()) {
            for (auto& ctxParameterEntry : paramList->parameterEntry()) {
                MethodCallParameter entry;
                entry.expression = Expression::fromContext(
                    ctxParameterEntry->expression());
                if (ctxParameterEntry->parameterLabel()) {
                    entry.label = ctxParameterEntry->parameterLabel()->getText();
                }
                // Caller-side `#x` transfer (Phase 1 of #68).
                if (ctxParameterEntry->REFERENCE()) {
                    entry.callerTransferred = true;
                }
                parameters.push_back(entry);
            }
        }
        // Form C call-site type args. Each typeArgument resolves through
        // CajetaType::fromContext under the active module's substitution
        // stack so a `<T>` referenced from inside a templated class body
        // still resolves to the bound T. A non-type (integer-constant)
        // argument — `m<8>(...)` — resolves to a CajetaConstantType,
        // exactly as the type-use site does for `Vector<T, 8>` / a
        // user template's `uint32 N` parameter (CajetaType.cpp).
        if (auto* targs = ctx->typeArguments()) {
            auto activeMod = CajetaModule::getActiveModule();
            for (auto* ta : targs->typeArgument()) {
                if (ta->integerLiteral() != nullptr) {
                    explicitMethodTypeArgs.push_back(CajetaConstantType::of(
                        CajetaConstantType::parseLiteral(ta->integerLiteral())));
                } else if (ta->typeType()) {
                    auto t = CajetaType::fromContext(ta->typeType(), activeMod);
                    if (t) explicitMethodTypeArgs.push_back(t);
                }
                // typeArgument's primitiveType alt is subsumed by typeType;
                // the wildcard `?` alt is not valid in a call-site arg list.
            }
        }
    }

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

    // When the receiver looks like `System.<something>` but `<something>` isn't
    // a known stream (stdout/stderr/stdin), return that misspelled name so
    // the caller can throw a diagnostic. Empty string means the receiver
    // isn't `System.<x>` shape and the call-site should proceed with regular
    // method resolution. (Static-method calls on a real `System` class
    // someday would fall through this path because the methodCall would be
    // on `System.foo(...)` directly, not `System.foo.bar(...)`.)
    static std::string detectSystemUnknownStream(const AbstractSyntaxNodePtr& receiver) {
        auto dot = dynamic_pointer_cast<DotExpression>(receiver);
        if (!dot) return "";
        const std::string& name = dot->getIdentifier();
        if (systemStreamFd(name) >= 0) return "";   // known stream, OK
        // Other recognized System.<ns> namespaces — env / property — also
        // bypass the unknown-stream diagnostic (their own intrinsic detector
        // dispatches them).
        if (name == "env" || name == "property") return "";
        const auto& dotChildren = const_cast<DotExpression*>(dot.get())->getChildren();
        if (dotChildren.empty()) return "";
        auto sys = dynamic_pointer_cast<IdentifierExpression>(dotChildren[0]);
        if (!sys) return "";
        if (sys->getTextValue() != "System") return "";
        return name;
    }

    // Detect `System.env` or `System.property` receiver shape for the
    // env-var / system-property intrinsics. Returns the namespace name
    // ("env" / "property") or empty string when the shape doesn't match.
    static std::string detectSystemNamespaceReceiver(const AbstractSyntaxNodePtr& receiver) {
        auto dot = dynamic_pointer_cast<DotExpression>(receiver);
        if (!dot) return "";
        const std::string& name = dot->getIdentifier();
        if (name != "env" && name != "property") return "";
        const auto& dotChildren = const_cast<DotExpression*>(dot.get())->getChildren();
        if (dotChildren.empty()) return "";
        auto sys = dynamic_pointer_cast<IdentifierExpression>(dotChildren[0]);
        if (!sys) return "";
        if (sys->getTextValue() != "System") return "";
        return name;
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
        auto& llvmCtx = *module->getLlvmContext();
        llvm::Value* v = argNode->generateCode(module);
        auto argExpr = std::dynamic_pointer_cast<Expression>(argNode);
        CajetaTypePtr argTy = argExpr ? argExpr->getResolvedType() : nullptr;
        if (!argTy && argExpr) {
            argExpr->resolveTypes(module);
            argTy = argExpr->getResolvedType();
        }
        // Load through l-values to the r-value: a String passed as a local
        // (alloca), an array element (`args[i]` — an ArrayIndex GEP whose slot
        // holds the heap `String*`), or a class field (a DotExpression GEP)
        // must become the heap String pointer, not the slot address.
        // loadIfLValue uses the AST's resolved type to load reference elements
        // as `ptr`; it leaves literal globals / r-values untouched. (The old
        // alloca-only load mishandled array elements and fields.)
        if (argExpr) {
            v = loadIfLValue(module, v, argExpr);
        } else if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
            v = builder->CreateLoad(a->getAllocatedType(), a);
        }
        // Post Phase 2b-β: a "String" arg is now a class
        // `cajeta.lang.String` instance pointer, not a raw `char*`.
        // The legacy runtime helpers (__cajeta_parse_i64,
        // __cajeta_parse_f64, __cajeta_parse_bool, __cajeta_log,
        // __cajeta_println, …) still take `const char*`, so unwrap
        // class String args here: GEP into `.bytes` slot (struct
        // index 1, the int8[] field), load the CajetaArray header
        // pointer, then GEP past its 8-byte count to land on the
        // first data byte. The literal codegen guarantees null
        // termination so any strlen-reader sees the right end.
        if (argTy) {
            auto cls = std::dynamic_pointer_cast<CajetaClass>(argTy);
            if (cls && cls->getQName()
                    && cls->getQName()->getTypeName() == "String"
                    && cls->getQName()->getPackageName() == "cajeta.lang"
                    && cls->getLlvmType()
                    && llvm::isa<llvm::StructType>(cls->getLlvmType())) {
                auto* structTy =
                    llvm::cast<llvm::StructType>(cls->getLlvmType());
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Value* bytesSlot = builder->CreateStructGEP(
                    structTy, v, 1, "strArg.bytes_slot");
                llvm::Value* bytesPtr = builder->CreateLoad(
                    ptrTy, bytesSlot, "strArg.bytes_ptr");
                v = builder->CreateInBoundsGEP(i8Ty, bytesPtr,
                    llvm::ConstantInt::get(i64Ty, 8),
                    "strArg.cstr");
            }
        }
        return v;
    }

    llvm::Value* MethodCallExpression::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& llvmCtx = *module->getLlvmContext();

        // ----- tryAs<T>() intrinsic (reified capture -> Optional<T>) -----
        // `recv.tryAs<Foo<int32>>()` checks recv's runtime reified instantiation
        // (reified-capture-spec.md §1/§4) and returns Optional<Foo<int32>>:
        // present, holding the SAME pointer, on a match; empty on mismatch or
        // null. Built on __cajeta_instanceof_named + the shared construction
        // helper (CajetaClass::heapConstruct). No control flow needed — the
        // Optional just stores (present, value): present = the match bit, value =
        // select(match, recv, null), then one Optional<T> construction. The held
        // value is a borrow (aliases recv's object; representation-identical
        // because cajeta monomorphizes), not a fresh allocation.
        if (methodCallName == "tryAs"
                && explicitMethodTypeArgs.size() == 1
                && explicitMethodTypeArgs[0]
                && parameters.empty()
                && !children.empty()) {
            auto recvExpr = std::dynamic_pointer_cast<Expression>(children[0]);
            if (recvExpr) {
                if (!recvExpr->getResolvedType()) recvExpr->resolveTypes(module);
                llvm::Value* raw = children[0]->generateCode(module);
                llvm::Value* objPtr = loadIfLValue(module, raw, recvExpr);
                CajetaTypePtr targetType = explicitMethodTypeArgs[0];
                auto optTmpl = std::dynamic_pointer_cast<CajetaClass>(
                    CajetaType::of("Optional", "cajeta.lang"));
                if (objPtr && objPtr->getType()->isPointerTy()
                        && optTmpl && optTmpl->isTemplate()) {
                    auto optInst = std::dynamic_pointer_cast<CajetaClass>(
                        optTmpl->instantiate({targetType}));
                    if (optInst) {
                        llvm::PointerType* ptrTy =
                            llvm::PointerType::get(llvmCtx, 0);
                        llvm::Value* nullPtr =
                            llvm::ConstantPointerNull::get(ptrTy);
                        llvm::Value* matchBit = llvm::ConstantInt::getFalse(
                            llvm::Type::getInt1Ty(llvmCtx));
                        if (llvm::Function* fn = module->getRuntimeFunction(
                                "__cajeta_instanceof_named")) {
                            llvm::Value* namePtr = builder->CreateGlobalString(
                                targetType->toCanonical(), "tryas.target");
                            llvm::Value* r = builder->CreateCall(
                                fn, {objPtr, namePtr}, "tryas.match");
                            matchBit = builder->CreateICmpNE(
                                r, llvm::ConstantInt::get(r->getType(), 0));
                        }
                        llvm::Value* value = builder->CreateSelect(
                            matchBit, objPtr, nullPtr, "tryas.val");
                        // boolean is i1 in cajeta, so matchBit is the present arg.
                        CajetaTypePtr boolTy = CajetaType::of("boolean");
                        std::vector<ParameterEntry> entries;
                        entries.push_back(ParameterEntry(boolTy, "", matchBit));
                        entries.push_back(ParameterEntry(targetType, "", value));
                        llvm::Value* opt =
                            optInst->heapConstruct(module, entries);
                        resolvedType = optInst;
                        return opt;
                    }
                }
            }
        }

        // ----- REFL-12: bounded reflection lowering -----
        // The bounded reflection entry points carry the bound `Shape` as an
        // explicit method type argument: `Class.heapInstance<Shape>(name)` and
        // `Class.subtypes<Shape>()`. The bound is the supertype the developer
        // already needs to use the result; surfacing it as `<Shape>` makes the
        // result type-checked / closure-scoped and hands the lean linker a
        // closed-world keep-token (the bound's subtype closure; see
        // plans/compiler/lean-linker-dce.md). Both lower the SAME way: append the
        // bound as a synthesized `Shape.class` literal so the call resolves to a
        // stdlib overload taking `Class<?> bound`, which runs the runtime
        // `leaf <: T` check via __cajeta_is_subtype. They differ only in whether
        // the `<Shape>` token survives the rewrite:
        //   - heapInstance<Shape>(name) -> heapInstance(name, Shape.class): KEEP
        //     the token — it binds the `Optional<T>` return type natively.
        //   - subtypes<Shape>()        -> subtypes(Shape.class):          DROP the
        //     token — the return is the wildcard `#Class<?>[]` (T is purely the
        //     keep-token + filter), and a concrete element type would re-trip the
        //     ClassObject borrow-drop that defers `forName<T>` (see Class.cajeta).
        // Guarded so the arg is injected exactly once.
        if (!boundedReflInjected
                && explicitMethodTypeArgs.size() == 1
                && explicitMethodTypeArgs[0]
                && !children.empty()) {
            // bounded by-name heapInstance<T>(name); the <T> token distinguishes
            // it from the by-index heapInstance(int32) primitive (no type arg).
            bool isBoundedHeapInstance =
                methodCallName == "heapInstance" && parameters.size() == 1;
            bool isBoundedSubtypes =
                methodCallName == "subtypes" && parameters.empty();
            // classesAnnotated<@A>(): inject A's canonical name as the String arg
            // and keep only @A-bearing classes.
            bool isAnnotatedToken =
                methodCallName == "classesAnnotated" && parameters.empty();
            if (isBoundedHeapInstance || isBoundedSubtypes || isAnnotatedToken) {
                if (auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(
                        children[0])) {
                    auto rt = CajetaType::of(recvId->getTextValue());
                    if (rt && rt->toCanonical() == "cajeta.reflect.Class") {
                        std::string tok =
                            explicitMethodTypeArgs[0]->toCanonical();
                        MethodCallParameter arg;
                        if (isAnnotatedToken) {
                            arg.expression =
                                std::make_shared<TextLiteralExpression>(
                                    "\"" + tok + "\"", LITERAL_TYPE_STRING);
                        } else {
                            arg.expression =
                                std::make_shared<ClassLiteralExpression>(
                                    tok, nullptr);
                        }
                        parameters.push_back(arg);
                        boundedReflInjected = true;
                        auto& keep = CajetaModule::reflectionKeep();
                        using RS = CajetaModule::ReflSite;
                        if (isAnnotatedToken) {
                            auto p = tok.rfind('.');
                            keep.sites.push_back({RS::Annotated,
                                p == std::string::npos ? tok : tok.substr(p + 1)});
                            explicitMethodTypeArgs.clear();
                        } else {
                            // REFL-12.3: bound = lean keep-token (subtype closure).
                            keep.sites.push_back({RS::BoundClosure, tok});
                            // subtypes' return is wildcard — drop the token so the
                            // call resolves to the non-templated overload.
                            if (isBoundedSubtypes) explicitMethodTypeArgs.clear();
                        }
                    }
                }
            }
        }

        // DCE Tier-0b: classify registry-consuming reflection from non-reflect
        // code (lean-linker-dce.md §3.2). Resolvable sites record a narrow site
        // descriptor; open ones set forcesAll. Must be complete — a missed site
        // lets the linker strip a class a later forName needs.
        if (!children.empty()) {
            static const std::set<std::string> kClassReflEntry = {
                "forName", "allClasses", "classesInPackage",
                "classesAnnotated", "subtypes", "heapInstance"};
            bool isClassEntry = false, isGetType = false;
            if (kClassReflEntry.count(methodCallName)) {
                if (auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(
                        children[0])) {
                    auto rt = CajetaType::of(recvId->getTextValue());
                    isClassEntry =
                        rt && rt->toCanonical() == "cajeta.reflect.Class";
                }
            } else if (methodCallName == "getType") {
                if (auto recvExpr =
                        std::dynamic_pointer_cast<Expression>(children[0])) {
                    if (!recvExpr->getResolvedType()) {
                        recvExpr->resolveTypes(module);
                    }
                    auto rt = recvExpr->getResolvedType();
                    isGetType = rt &&
                        rt->toCanonical() == "cajeta.reflect.TemplateArgument";
                }
            }
            bool callerInReflect = false;
            if (auto cm = module->getCurrentMethod()) {
                if (auto owner = cm->getParent()) {
                    callerInReflect =
                        owner->toCanonical().rfind("cajeta.reflect.", 0) == 0;
                }
            }
            if ((isClassEntry || isGetType) && !callerInReflect) {
                auto& keep = CajetaModule::reflectionKeep();
                // a constant-folded String arg → narrow; else open
                auto stringLiteralArg = [&](int i) -> std::string {
                    if (i >= (int) parameters.size()) return "";
                    auto lit = std::dynamic_pointer_cast<TextLiteralExpression>(
                        parameters[i].expression);
                    if (!lit ||
                            lit->getLiteralType() != LITERAL_TYPE_STRING) {
                        return "";
                    }
                    std::string v = lit->getRawValue();  // includes quotes
                    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                        return v.substr(1, v.size() - 2);
                    }
                    return "";
                };
                auto shortName = [](const std::string& canon) {
                    auto p = canon.rfind('.');
                    return p == std::string::npos ? canon : canon.substr(p + 1);
                };
                using RS = CajetaModule::ReflSite;
                using M = CajetaModule;
                if (isGetType) {
                    M::noteForceAll("TemplateArgument.getType() — resolves any "
                        "template-argument type by name; bound it with a concrete "
                        "Class<T> instead");
                } else if (methodCallName == "forName") {
                    std::string s = stringLiteralArg(0);
                    if (!s.empty()) keep.sites.push_back({RS::ForNameLiteral, s});
                    else M::noteForceAll("forName(<non-literal>) — pass a string "
                        "literal, or use Class.subtypes<Base>() for a bounded set");
                } else if (methodCallName == "classesInPackage") {
                    std::string s = stringLiteralArg(0);
                    if (!s.empty()) keep.sites.push_back({RS::PackageLiteral, s});
                    else M::noteForceAll("classesInPackage(<non-literal>) — pass a "
                        "package-name string literal");
                } else if (methodCallName == "classesAnnotated") {
                    std::string s = stringLiteralArg(0);
                    if (!s.empty())
                        keep.sites.push_back({RS::Annotated, shortName(s)});
                    else M::noteForceAll("classesAnnotated(<non-literal>) — pass an "
                        "annotation token classesAnnotated<@A>() or a name literal");
                } else if (methodCallName == "heapInstance"
                        || methodCallName == "subtypes") {
                    // bounded form already recorded BoundClosure above; the
                    // by-index heapInstance(int32) isn't a Class-static call so
                    // it never reaches here
                    if (!boundedReflInjected)
                        M::noteForceAll(methodCallName + "(...) without a <T> bound "
                            "— add a type bound, e.g. subtypes<Base>()");
                } else {  // allClasses + any other enumerator
                    M::noteForceAll(methodCallName + "() — enumerates the whole "
                        "registry; use Class.subtypes<Base>() for a bounded set");
                }
            }
        }

        // ----- Buffer<T>.elementBytes() intrinsic -----
        // Cajeta has no source-level sizeof, but the Buffer<T> device methods
        // (alloc/upload/download) need n*sizeof(T) byte counts. This call is
        // lowered to a constant: the target DataLayout byte size of T, where
        // T is the element type of the instantiated Buffer<T> that owns the
        // current method. The declared placeholder body is never emitted as a
        // real call. Gated on the enclosing class being an xpu.core Buffer
        // instantiation so it can't shadow a same-named method elsewhere.
        if (methodCallName == "elementBytes") {
            if (auto cm = module->getCurrentMethod()) {
                auto parent = cm->getParent();
                if (parent && parent->isInstantiation()
                        && parent->toCanonical().rfind(
                               "cajeta.gpu.KernelBuffer", 0) == 0
                        && !parent->getTypeArguments().empty()) {
                    auto elemT = parent->getTypeArguments()[0];
                    llvm::Type* lt = elemT ? elemT->getLlvmType() : nullptr;
                    uint64_t sz = 0;
                    if (lt) {
                        sz = module->getLlvmModule()->getDataLayout()
                                 .getTypeAllocSize(lt);
                    }
                    if (auto u64 = CajetaType::of("uint64")) resolvedType = u64;
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(llvmCtx), sz);
                }
            }
        }

        // ----- REFL-11: constant-fold statically-known reflection -----
        // Fold `Class.of(<ident>).<accessor>(...)` to a compile-time constant or
        // a direct field load when <ident> is a `final`-class-typed identifier —
        // so its dynamic type provably equals its static type and the layout /
        // metadata the fold bakes in is exactly what the runtime reflective
        // native would read (spec Strategy 5). Restricting the receiver to an
        // identifier means there are no side effects to preserve when the inner
        // `Class.of(...)` call is elided. A sealed class's private field is left
        // un-folded so the runtime IllegalAccessException path is preserved.
        if (!children.empty()) {
            // Identify `cajeta.reflect.Class.of(<ident>)`. `Class.of(arg)` is a
            // qualified static call: its receiver `Class` is a child. We cannot
            // read of()'s RETURN type without generating the call (the very call
            // we want to elide), but the receiver identifier resolves statically
            // by name — CajetaType::of(text) gives cajeta.reflect.Class. That,
            // plus method "of" + one arg, pins it down precisely.
            auto innerCall =
                std::dynamic_pointer_cast<MethodCallExpression>(children[0]);
            bool isClassOf = false;
            if (innerCall && innerCall->getMethodCallName() == "of"
                    && innerCall->getParameters().size() == 1
                    && !innerCall->getChildren().empty()) {
                if (auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(
                        innerCall->getChildren()[0])) {
                    auto rt = CajetaType::of(recvId->getTextValue());
                    isClassOf =
                        rt && rt->toCanonical() == "cajeta.reflect.Class";
                }
            }
            if (isClassOf) {
                {
                    ExpressionPtr ofArg = innerCall->getParameters()[0].expression;
                    auto ofIdent =
                        std::dynamic_pointer_cast<IdentifierExpression>(ofArg);
                    if (ofIdent) {
                        if (!ofArg->getResolvedType()) ofArg->resolveTypes(module);
                        auto K = std::dynamic_pointer_cast<CajetaClass>(
                            ofArg->getResolvedType());
                        // Exact-type guard: only a `final` class is provably its
                        // own runtime type through an identifier binding.
                        if (K && K->getModifiers().count(FINAL) > 0) {
                            // (a) integer metadata accessors -> ConstantInt.
                            if (parameters.empty()) {
                                auto* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                                if (methodCallName == "getFieldCount") {
                                    resolvedType = CajetaType::of("int32");
                                    return llvm::ConstantInt::get(i32Ty,
                                        (uint64_t)(uint32_t)
                                            K->getPropertyList().size());
                                }
                                if (methodCallName == "getMethodCount") {
                                    resolvedType = CajetaType::of("int32");
                                    return llvm::ConstantInt::get(i32Ty,
                                        (uint64_t)(uint32_t)
                                            K->getMethodList().size());
                                }
                                if (methodCallName == "getParentCount") {
                                    resolvedType = CajetaType::of("int32");
                                    return llvm::ConstantInt::get(i32Ty,
                                        (uint64_t)(uint32_t)
                                            K->getSuperClasses().size());
                                }
                                if (methodCallName == "getModifierFlags") {
                                    int32_t bits = 0;
                                    for (auto m : K->getModifiers())
                                        bits |= (int32_t) m;
                                    resolvedType = CajetaType::of("int32");
                                    return llvm::ConstantInt::get(i32Ty,
                                        (uint64_t)(uint32_t) bits);
                                }
                                if (methodCallName == "getInstanceSize") {
                                    uint64_t sz = 0;
                                    if (auto lt = K->getLlvmType())
                                        sz = module->getLlvmModule()
                                                 ->getDataLayout()
                                                 .getTypeAllocSize(lt);
                                    resolvedType = CajetaType::of("int64");
                                    return llvm::ConstantInt::get(
                                        llvm::Type::getInt64Ty(llvmCtx), sz);
                                }
                            }
                            // (b) typed primitive field load -> direct
                            //     (obj + byteOffset) load, byte-identical to the
                            //     reflective native. Shape:
                            //     Class.of(g).getInt32(g, <literal index>).
                            const char* wantFieldType = nullptr;
                            if (methodCallName == "getInt32")
                                wantFieldType = "int32";
                            else if (methodCallName == "getInt64")
                                wantFieldType = "int64";
                            else if (methodCallName == "getFloat32")
                                wantFieldType = "float32";
                            else if (methodCallName == "getFloat64")
                                wantFieldType = "float64";
                            if (wantFieldType && parameters.size() == 2) {
                                // arg0 must be the SAME identifier we proved
                                // exact; arg1 a non-negative decimal literal.
                                auto objIdent =
                                    std::dynamic_pointer_cast<IdentifierExpression>(
                                        parameters[0].expression);
                                auto idxLit =
                                    std::dynamic_pointer_cast<
                                        IntegerLiteralExpression>(
                                            parameters[1].expression);
                                bool sameRecv = objIdent
                                    && objIdent->getTextValue()
                                           == ofIdent->getTextValue();
                                int64_t litIdx = -1;
                                if (idxLit && idxLit->getIntegerLiteralType()
                                        == INTEGER_LITERAL_TYPE_DECIMAL) {
                                    bool ok = true;
                                    std::string digits;
                                    for (char ch : idxLit->getRawValue()) {
                                        if (ch == '_') continue;
                                        if (ch < '0' || ch > '9') { ok = false; break; }
                                        digits.push_back(ch);
                                    }
                                    if (ok && !digits.empty()) {
                                        try { litIdx = std::stoll(digits); }
                                        catch (...) { litIdx = -1; }
                                    }
                                }
                                auto& props = K->getPropertyList();
                                if (sameRecv && litIdx >= 0
                                        && (size_t) litIdx < props.size()) {
                                    auto it = props.begin();
                                    std::advance(it, (size_t) litIdx);
                                    StructurePropertyPtr p = *it;
                                    bool sealed =
                                        K->getModifiers().count(REFLECT_SEALED) > 0;
                                    bool priv =
                                        p->getModifiers().count(PRIVATE) > 0;
                                    bool typeMatch = p->getType()
                                        && p->getType()->toCanonical()
                                               == wantFieldType;
                                    int llvmIdx = K->getFieldLlvmIndex(p);
                                    auto* instStruct =
                                        llvm::dyn_cast_or_null<llvm::StructType>(
                                            K->getLlvmType());
                                    if (!(sealed && priv) && typeMatch
                                            && !p->isStatic() && instStruct
                                            && llvmIdx >= 0
                                            && (unsigned) llvmIdx
                                                   < instStruct->getNumElements()) {
                                        const llvm::StructLayout* layout =
                                            module->getLlvmModule()
                                                ->getDataLayout()
                                                .getStructLayout(instStruct);
                                        uint64_t off = layout->getElementOffset(
                                            (unsigned) llvmIdx);
                                        // generateCode on a local identifier
                                        // yields its alloca (l-value: the slot
                                        // holding the object pointer), not the
                                        // pointer itself. Load through it so the
                                        // GEP base is the object, mirroring the
                                        // normal argument-lowering coercion.
                                        llvm::Value* objSlot =
                                            parameters[0].expression
                                                ->generateCode(module);
                                        llvm::Value* objVal = objSlot
                                            ? builder->CreateLoad(
                                                  llvm::PointerType::get(
                                                      llvmCtx, 0),
                                                  objSlot, "refl.fold.obj")
                                            : nullptr;
                                        if (objVal) {
                                            llvm::Value* fieldPtr =
                                                builder->CreateGEP(
                                                    llvm::Type::getInt8Ty(llvmCtx),
                                                    objVal,
                                                    llvm::ConstantInt::get(
                                                        llvm::Type::getInt64Ty(
                                                            llvmCtx),
                                                        off),
                                                    "refl.fold.fieldptr");
                                            resolvedType = p->getType();
                                            return builder->CreateLoad(
                                                p->getType()->getLlvmType(),
                                                fieldPtr, "refl.fold.load");
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // CajetaXPU launch borrow scope (§3.5 / §11). A `kernel.launch(...)`
        // borrows each Buffer arg until the next Stream.sync() /
        // Event.waitHost(); freeing a still-borrowed buffer is a use-after-
        // free of memory an in-flight kernel references. Gated strictly on
        // the receiver being an xpu.core Stream/Event/Buffer so it never
        // touches a same-named method on an unrelated type.
        if ((methodCallName == "sync" || methodCallName == "waitHost" ||
             methodCallName == "free") && !children.empty()) {
            if (auto recvId =
                    std::dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                if (auto sc = module->getScopeStack().peek()) {
                    std::string canonical;
                    const std::string& recv = recvId->getTextValue();
                    if (sc->containsField(recv)) {
                        if (auto f = sc->getField(recv)) {
                            if (f->getType()) canonical = f->getType()->toCanonical();
                        }
                    }
                    bool isStream = canonical == "cajeta.gpu.KernelStream";
                    bool isEvent  = canonical == "cajeta.gpu.Event";
                    bool isBuffer =
                        canonical.rfind("cajeta.gpu.KernelBuffer", 0) == 0;
                    if ((isStream && methodCallName == "sync") ||
                        (isEvent && methodCallName == "waitHost")) {
                        sc->releaseLaunchBorrows();
                    } else if (isBuffer && methodCallName == "free" &&
                               sc->isLaunchBorrowed(recv)) {
                        throw Exception(
                            "buffer '" + recv + "' is freed while a launch still "
                            "references it — sync the stream (Stream.sync()) "
                            "before freeing", "XPU-K02");
                    }
                }
            }
        }

        // `super(args)` ctor delegation. Look up the enclosing class's
        // first declared parent, resolve a constructor matching the args,
        // invoke it on `this` with isConstructor=true and forceDirectCall
        // (ctor invocation is never vtable-dispatched anyway, but the
        // flag keeps the path explicit). Method::generateCode's implicit
        // super-call only fires when a parent's no-arg ctor resolves, so
        // an explicit super(args) doesn't double-init unless the parent
        // happens to also have a no-arg ctor; we suppress that case here
        // by recording in the module that an explicit super-call has run
        // in this constructor body (TODO when needed). For Gap 6's test
        // shape (parent has only args-ctor), the implicit path is a
        // no-op anyway.
        if (superCtorCall) {
            if (module->getStructureStack().empty()) {
                throw Exception("`super(...)` used outside of a class ctor",
                    "CAJETA_ERROR_SUPER_OUTSIDE_CLASS");
            }
            auto here = std::dynamic_pointer_cast<CajetaClass>(
                module->getStructureStack().back());
            if (!here || here->getSuperClasses().empty()) {
                throw Exception("`super(...)` used in a class with no parent",
                    "CAJETA_ERROR_SUPER_NO_PARENT");
            }
            CajetaClassPtr parentCls = here->getSuperClasses().front();
            // Build the ParameterEntry list (same shape as the normal
            // dispatch path further down). Lift expressions to IR and
            // pin resolved types so resolveMethod's lookup keys match.
            //
            // Arg expressions that resolve to l-values (an
            // IdentifierExpression referencing a ctor parameter, a
            // local-variable alloca, a DotExpression's field GEP)
            // produce a pointer value from generateCode — the
            // ctor's signature wants the loaded scalar/pointer, not
            // the slot address. loadIfLValue is the same coercion
            // the regular invokeMethod path applies one frame down;
            // historically the super-ctor branch built ParameterEntry
            // before going through that coercion, so passing a ctor
            // parameter to super() emitted (ptr-of-alloca, ...) and
            // tripped the JIT verifier with "Call parameter type
            // does not match function signature!". Apply the coercion
            // here so super(bv) — where bv is a ctor formal — works
            // alongside super(literal).
            std::vector<ParameterEntry> entries;
            for (auto& p : parameters) {
                if (p.expression && !p.expression->getResolvedType()) {
                    p.expression->resolveTypes(module);
                }
                llvm::Value* v = p.expression ? p.expression->generateCode(module) : nullptr;
                CajetaTypePtr t = p.expression ? p.expression->getResolvedType() : nullptr;
                if (v && p.expression) {
                    auto exprAst = std::dynamic_pointer_cast<Expression>(p.expression);
                    v = loadIfLValue(module, v, exprAst);
                }
                entries.emplace_back(t, p.label, v);
            }
            // The receiver is `this` (the same instance). The parent
            // ctor writes its inherited slots through `this`.
            auto scope = module->getScopeStack().peek();
            FieldPtr thisField = scope ? scope->getField("this") : nullptr;
            if (!thisField) {
                throw Exception("`super(...)` used in a static context with no `this`",
                    "CAJETA_ERROR_SUPER_IN_STATIC");
            }
            llvm::Value* thisValue = builder->CreateLoad(
                thisField->getOrCreateAllocation()->getAllocatedType(),
                thisField->getOrCreateAllocation());
            // Per-parent sub-object adjustment (Gap 8). For the FIRST parent
            // offset is 0 (shares primary vtable). For non-first parents
            // we shift the receiver to the parent's sub-object start so
            // the parent ctor's pre-compiled IR uses correct slot indices.
            uint64_t off = here->getSubObjectByteOffset(parentCls.get());
            if (off != 0) {
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(
                    *module->getLlvmContext());
                thisValue = builder->CreateInBoundsGEP(i8Ty, thisValue,
                    llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(*module->getLlvmContext()),
                        off),
                    "super_ctor_subobj");
            }
            std::string ctorName = parentCls->getQName()->getTypeName();
            return parentCls->invokeMethod(ctorName, entries,
                /*isConstructor=*/true, thisValue, /*callerModule=*/module,
                /*forceDirectCall=*/true);
        }

        // ----- Indirect call through a function-typed local -----
        // `add(3, 4)` where `add` was declared as `(int32, int32) -> int32`.
        // The local's slot holds a `ptr` to a closure record
        // `{ ptr fn, ptr captures }` (L2 ABI). Load the closure, extract
        // both fields, and indirect-dispatch with captures prepended to the
        // user args. Matches when the call is bare (no receiver) AND a
        // scope lookup of methodCallName yields a function-typed field.
        // See docs/specification/lang/Lambdas.md.
        if (children.empty() && !module->getScopeStack().isEmpty()) {
            auto scope = module->getScopeStack().peek();
            FieldPtr field = scope ? scope->getField(methodCallName) : nullptr;
            if (field) {
                // cajeta-ir Unit 4b: a specialized instance binds the (dropped)
                // function-typed parameter to a known function. Dispatch direct —
                // no closure record, no indirect load.
                if (auto bound = dynamic_pointer_cast<BoundClosureField>(field)) {
                    auto fnType = dynamic_pointer_cast<CajetaFunctionType>(
                        bound->getType());
                    if (fnType) {
                        return emitClosureCall(module, /*closurePtr=*/nullptr,
                            fnType, parameters, resolvedType,
                            bound->getBoundFunction());
                    }
                }
                auto fnType = dynamic_pointer_cast<CajetaFunctionType>(field->getType());
                if (fnType) {
                    // The field's slot holds a `ptr` to the closure record;
                    // load it and dispatch through the shared closure ABI.
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::AllocaInst* slot = field->getOrCreateAllocation();
                    llvm::Value* closurePtr = builder->CreateLoad(
                        ptrTy, slot, "closure_ptr");
                    return emitClosureCall(module, closurePtr, fnType,
                                           parameters, resolvedType);
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
        // the canonical name of a registered view.
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
                        bool isVar = CajetaView::isVariableSize(p);
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

                // Caller-side `#bytes` transfer (Phase 1 of #68). The
                // inline view-construction path returns before the
                // general transfer block below, so handle the
                // caller-side acknowledgement here. The owning-vs-borrow
                // view distinction itself is decided downstream by
                // LocalVariableDeclaration (which also reads
                // callerTransferred); this block just deactivates the
                // bytes local's drop entry when the caller wrote `#`.
                if (parameters[0].callerTransferred) {
                    if (auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                            parameters[0].expression)) {
                        if (auto scope = module->getScopeStack().peek()) {
                            FieldPtr field = scope->getField(idExpr->getTextValue());
                            if (field) {
                                if (llvm::Value* entry = field->getDropEntry()) {
                                    if (llvm::Function* mark = module->getRuntimeFunction(
                                            "__cajeta_drop_mark_inactive")) {
                                        builder->CreateCall(mark, {entry});
                                    }
                                }
                            }
                        }
                    }
                }

                return dataPtr;
            }
        }

        // ----- Bare class-construction syntax rejected (docs/specification/lang/UnifiedClasses.md P1b) -----
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
            // ----- System.env.<method>(...) / System.property.<method>(...) -----
            // OS environment variables (env) and process-scoped string
            // properties (property — populated by the binary's -Dkey=value
            // CLI args at startup). Both expose `get(String)` returning a
            // class String and `set(String, String)` returning void.
            {
                std::string sysNs = detectSystemNamespaceReceiver(children[0]);
                if (!sysNs.empty()) {
                    llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);

                    if (methodCallName == "get" && parameters.size() == 1) {
                        std::string fnName = "__cajeta_" + sysNs + "_get";
                        llvm::Function* getFn = module->getRuntimeFunction(fnName);
                        if (!getFn) return nullptr;
                        llvm::Value* nameArg = loadStringArg(module, parameters[0].expression);
                        llvm::Value* cstrResult = builder->CreateCall(getFn, {nameArg},
                            std::string("system.") + sysNs + ".get.cstr");

                        // Wrap the returned char* into a class String pointer.
                        // Layout: { ptr vtable, ptr bytes, i32 byteLength, i32 mode,
                        // i32 cachedCpLength }. Returns null when the runtime gave
                        // a null pointer (env var not set, property absent) — the
                        // cajeta-side caller compares against null normally.
                        auto stringType = CajetaType::of("String");
                        auto stringClass = dynamic_pointer_cast<CajetaClass>(stringType);
                        if (!stringClass || !stringClass->getLlvmType()) {
                            return cstrResult;
                        }
                        llvm::Type* stringStructTy = stringClass->getLlvmType();

                        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
                        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(
                            llvmCtx, "system." + sysNs + ".null", curFn);
                        llvm::BasicBlock* wrapBB = llvm::BasicBlock::Create(
                            llvmCtx, "system." + sysNs + ".wrap", curFn);
                        llvm::BasicBlock* joinBB = llvm::BasicBlock::Create(
                            llvmCtx, "system." + sysNs + ".join", curFn);

                        llvm::Value* isNull = builder->CreateICmpEQ(cstrResult,
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy)));
                        builder->CreateCondBr(isNull, nullBB, wrapBB);

                        // Null arm — yield a null String pointer.
                        builder->SetInsertPoint(nullBB);
                        llvm::Value* nullStr = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy));
                        builder->CreateBr(joinBB);

                        // Non-null arm — wrap into a class String via the same
                        // shape BinaryOpExpression's `+` concat uses for its
                        // char* result. Header: { i64 count, char[count + 1] }.
                        builder->SetInsertPoint(wrapBB);
                        llvm::Function* strlenFn = module->getRuntimeFunction("__cajeta_str_len");
                        llvm::Value* lenI64 = builder->CreateCall(strlenFn, {cstrResult},
                            "system.len");
                        llvm::Value* lenI32 = builder->CreateIntCast(lenI64, i32Ty,
                            /*isSigned=*/true);
                        llvm::Value* arrSize = builder->CreateAdd(lenI64,
                            llvm::ConstantInt::get(i64Ty, 9));
                        llvm::FunctionType* allocTy = llvm::FunctionType::get(
                            ptrTy, {i64Ty}, false);
                        llvm::FunctionCallee allocFn =
                            module->getLlvmModule()->getOrInsertFunction(
                                "__cajeta_alloc", allocTy);
                        llvm::Value* arrPtr = builder->CreateCall(allocFn, {arrSize});
                        builder->CreateMemSet(arrPtr,
                            llvm::ConstantInt::get(i8Ty, 0),
                            arrSize, llvm::MaybeAlign(8));
                        builder->CreateStore(lenI64, arrPtr);
                        llvm::Value* dataPtr = builder->CreateInBoundsGEP(i8Ty,
                            arrPtr, llvm::ConstantInt::get(i64Ty, 8));
                        builder->CreateMemCpy(dataPtr, llvm::MaybeAlign(1),
                            cstrResult, llvm::MaybeAlign(1), lenI64);

                        const llvm::DataLayout& dl =
                            module->getLlvmModule()->getDataLayout();
                        llvm::Constant* sSize = llvm::ConstantInt::get(i64Ty,
                            dl.getTypeAllocSize(stringStructTy));
                        llvm::Value* sPtr = MemoryManager::createMallocInstruction(
                            module, sSize, builder->GetInsertBlock());

                        llvm::Constant* vtableRef = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy));
                        if (auto* vt = stringClass->getVirtualTableGlobal()) {
                            vtableRef = CajetaModule::ensureGlobalInModule(
                                module->getLlvmModule(), vt);
                        }
                        builder->CreateStore(vtableRef,
                            builder->CreateStructGEP(stringStructTy, sPtr, 0));
                        builder->CreateStore(arrPtr,
                            builder->CreateStructGEP(stringStructTy, sPtr, 1));
                        builder->CreateStore(lenI32,
                            builder->CreateStructGEP(stringStructTy, sPtr, 2));
                        builder->CreateStore(llvm::ConstantInt::get(i32Ty, 0),
                            builder->CreateStructGEP(stringStructTy, sPtr, 3));
                        builder->CreateStore(llvm::ConstantInt::get(i32Ty, -1),
                            builder->CreateStructGEP(stringStructTy, sPtr, 4));
                        builder->CreateBr(joinBB);

                        // Join the two arms into one ptr-typed phi.
                        builder->SetInsertPoint(joinBB);
                        llvm::PHINode* phi = builder->CreatePHI(ptrTy, 2,
                            "system." + sysNs + ".get");
                        phi->addIncoming(nullStr, nullBB);
                        phi->addIncoming(sPtr, wrapBB);
                        return phi;
                    }
                    if (methodCallName == "set" && parameters.size() == 2) {
                        std::string fnName = "__cajeta_" + sysNs + "_set";
                        llvm::Function* setFn = module->getRuntimeFunction(fnName);
                        if (!setFn) return nullptr;
                        llvm::Value* nameArg  = loadStringArg(module, parameters[0].expression);
                        llvm::Value* valueArg = loadStringArg(module, parameters[1].expression);
                        return builder->CreateCall(setFn, {nameArg, valueArg});
                    }
                    // Unknown method on a known namespace — let it fall through;
                    // resolveMethod will throw a clean error on the missing
                    // method instead of us synthesizing one here.
                }
            }

            // Catch the `System.<unknown>.<method>(...)` shape — usually
            // `System.out.println(...)` from a Java reflex — before the
            // call-resolution path drops it silently. Diag-hint suggests
            // the correct cajeta receiver via Levenshtein over the known
            // stream names.
            std::string sysMisspelling = detectSystemUnknownStream(children[0]);
            if (!sysMisspelling.empty()) {
                std::string hint;
                if (module->getFlags().diagHints) {
                    std::vector<std::string> candidates = {
                        "stdout", "stderr", "stdin"
                    };
                    auto suggestions = cajeta::pickSimilar(sysMisspelling, candidates,
                        /*maxDistance=*/3);
                    hint = cajeta::formatDidYouMean(suggestions);
                }
                throw Exception(
                    "no `System." + sysMisspelling
                    + "` — cajeta exposes only `System.stdout`, "
                    "`System.stderr`, and `System.stdin` as I/O streams."
                    + hint,
                    "CAJETA_ERROR_UNKNOWN_SYSTEM_STREAM");
            }
            int streamFd = detectSystemStreamReceiver(children[0]);
            if (streamFd >= 0) {
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);

                llvm::Value* streamArg = llvm::ConstantInt::get(i32Ty, streamFd);

                // Multi-arg `print(fmt, x, y, ...)` / `println(fmt, x, y, ...)`
                // — convenience formatting that walks `{}` placeholders in the
                // first arg and substitutes each subsequent arg. Each non-
                // String arg is stringified through the same runtime helpers
                // the single-arg dispatch uses (__cajeta_i64_to_str /
                // __cajeta_f64_to_str / __cajeta_bool_to_str). The resulting
                // char** is stack-allocated and passed to __cajeta_log
                // (print) or __cajeta_logln (println).
                if ((methodCallName == "print" || methodCallName == "println")
                        && parameters.size() >= 2) {
                    const char* runtimeName = methodCallName == "println"
                        ? "__cajeta_logln" : "__cajeta_log";
                    llvm::Function* logFn = module->getRuntimeFunction(runtimeName);
                    if (logFn) {
                        // First arg is the format string. Reuse loadStringArg
                        // (unwraps class String → char*) so the runtime helper
                        // sees a plain C string.
                        llvm::Value* fmt = loadStringArg(module, parameters[0].expression);
                        size_t argCount = parameters.size() - 1;

                        // char** argv = alloca [argCount x ptr]
                        llvm::Value* argv = builder->CreateAlloca(
                            ptrTy,
                            llvm::ConstantInt::get(i64Ty, argCount),
                            "printtmpl.argv");

                        for (size_t ai = 0; ai < argCount; ++ai) {
                            llvm::Value* raw = loadStringArg(module,
                                parameters[ai + 1].expression);
                            llvm::Type* rawTy = raw->getType();
                            llvm::Value* cstr = nullptr;
                            if (rawTy->isPointerTy()) {
                                // Already a char* (String unwrap or null).
                                cstr = raw;
                            } else if (rawTy->isIntegerTy(1)) {
                                llvm::Value* widened = builder->CreateZExt(raw, i32Ty);
                                llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                                cstr = builder->CreateCall(fn, {widened},
                                    "printtmpl.bool");
                            } else if (rawTy->isIntegerTy()) {
                                llvm::Value* widened = builder->CreateIntCast(raw, i64Ty, /*isSigned=*/true);
                                llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                                cstr = builder->CreateCall(fn, {widened},
                                    "printtmpl.i64");
                            } else if (rawTy->isFloatingPointTy()) {
                                llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                                if (rawTy != f64Ty) {
                                    raw = builder->CreateFPCast(raw, f64Ty);
                                }
                                llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                                cstr = builder->CreateCall(fn, {raw},
                                    "printtmpl.f64");
                            } else {
                                // Last-resort: emit a `null` placeholder
                                // pointer so the runtime substitutes "null"
                                // rather than crashing. Hits classes that
                                // don't expose toString yet.
                                cstr = llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            }
                            llvm::Value* slot = builder->CreateGEP(
                                ptrTy, argv,
                                llvm::ConstantInt::get(i64Ty, ai),
                                std::string("printtmpl.slot.") + std::to_string(ai));
                            builder->CreateStore(cstr, slot);
                        }

                        llvm::Value* argcArg = llvm::ConstantInt::get(
                            i64Ty, (uint64_t) argCount);
                        return builder->CreateCall(logFn,
                            {streamArg, fmt, argcArg, argv});
                    }
                }

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
                    if (!arrType) {
                        // Java/C-style `printf(fmt, x)` with raw scalar args
                        // is not cajeta's contract — the second arg must be a
                        // String[] of substitutions. Surface a clear error
                        // instead of silently dropping the call. String
                        // concatenation (`fmt + value`) is the usual fix.
                        std::string actualType = "?";
                        if (argsExpr && argsExpr->getResolvedType()) {
                            actualType = argsExpr->getResolvedType()->toCanonical();
                        }
                        std::string msg =
                            "System." + std::string(streamFd == 2 ? "stderr" : "stdout")
                            + ".printf expects (String fmt, String[] args); got second "
                            "argument of type `" + actualType
                            + "`. Build a String[] of the substitutions, or use "
                            "concatenation: `System.stdout.println(\"x=\" + value);`.";
                        throw Exception(msg, "CAJETA_ERROR_PRINTF_BAD_ARGS");
                    }
                    llvm::Type* hdrTy = arrType->getLlvmType();
                    llvm::Value* sizePtr = builder->CreateStructGEP(hdrTy, argsHdr,
                        CajetaArray::SIZE_FIELD_INDEX);
                    llvm::Value* count = builder->CreateLoad(i64Ty, sizePtr);
                    llvm::Value* dataPtr = builder->CreateStructGEP(hdrTy, argsHdr,
                        CajetaArray::DATA_FIELD_INDEX);

                    // Post Phase 2b-β: each element of `String[]` is a
                    // class String pointer (not a `char*`). The runtime
                    // helper `__cajeta_log` reads argv[i] as `const
                    // char*` and would feed the class String struct's
                    // raw bytes through strlen, printing the vtable
                    // word + payload prefix as ASCII (garbage). Build
                    // a parallel char** array on the stack, unwrap each
                    // class String to its data pointer, and pass that
                    // to __cajeta_log instead.
                    //
                    // Array slot stride note: `String[]` allocates
                    // sizeof(class String) ≈ 32 bytes per slot today
                    // (per CajetaArray::getElementLlvmType returning
                    // the body struct type), but writers store an
                    // 8-byte literal pointer into each slot's first
                    // word. Use the array's struct-shaped GEP (matching
                    // what `args[i] = "..."` codegen emits) rather
                    // than a pointer-stride GEP into the raw data
                    // region — that way reads land at the same slot
                    // boundaries the writes used.
                    CajetaTypePtr elemTy = arrType->getElementType();
                    auto elemClass = std::dynamic_pointer_cast<CajetaClass>(elemTy);
                    bool elemsAreClassString = elemClass
                        && elemClass->getQName()
                        && elemClass->getQName()->getTypeName() == "String"
                        && elemClass->getQName()->getPackageName() == "cajeta.lang";
                    if (elemsAreClassString) {
                        llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);
                        llvm::Type* stringStructTy = elemClass->getLlvmType();
                        if (!stringStructTy
                                || !llvm::isa<llvm::StructType>(stringStructTy)) {
                            return builder->CreateCall(fn, {streamArg, fmt, count, dataPtr});
                        }
                        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
                        llvm::Value* cstrArr = builder->CreateAlloca(
                            ptrTy, count, "printf.cstrs");
                        llvm::BasicBlock* condBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.loop.cond", parentFn);
                        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.loop.body", parentFn);
                        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.loop.done", parentFn);
                        llvm::AllocaInst* iSlot = builder->CreateAlloca(
                            i64Ty, nullptr, "printf.i");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i64Ty, 0), iSlot);
                        builder->CreateBr(condBB);
                        builder->SetInsertPoint(condBB);
                        llvm::Value* i = builder->CreateLoad(i64Ty, iSlot, "printf.i.cur");
                        llvm::Value* cmp = builder->CreateICmpULT(i, count, "printf.i.lt");
                        builder->CreateCondBr(cmp, bodyBB, doneBB);
                        builder->SetInsertPoint(bodyBB);
                        // strPtrSlot = &argsHdr->data[i] via the array
                        // struct's own GEP — gets the per-slot stride
                        // right regardless of the element body size.
                        llvm::Value* strPtrSlot = builder->CreateGEP(
                            hdrTy, argsHdr,
                            { llvm::ConstantInt::get(i64Ty, 0),
                              llvm::ConstantInt::get(
                                  llvm::Type::getInt32Ty(llvmCtx),
                                  CajetaArray::DATA_FIELD_INDEX),
                              i },
                            "printf.strSlot");
                        // Reader: each slot's FIRST word holds the
                        // class-String pointer (writers store a `ptr`
                        // into the slot via the same GEP). Load that
                        // word as ptr.
                        llvm::Value* strPtr = builder->CreateLoad(
                            ptrTy, strPtrSlot, "printf.strPtr");
                        llvm::Value* isNull = builder->CreateICmpEQ(strPtr,
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy)),
                            "printf.isNull");
                        llvm::BasicBlock* extractBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.extract", parentFn);
                        llvm::BasicBlock* storeNullBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.storeNull", parentFn);
                        llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.after", parentFn);
                        builder->CreateCondBr(isNull, storeNullBB, extractBB);
                        builder->SetInsertPoint(extractBB);
                        llvm::Value* bytesSlot = builder->CreateStructGEP(
                            stringStructTy, strPtr, 1, "printf.bytesSlot");
                        llvm::Value* bytesPtr = builder->CreateLoad(
                            ptrTy, bytesSlot, "printf.bytesPtr");
                        llvm::Value* cstr = builder->CreateInBoundsGEP(
                            i8Ty, bytesPtr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "printf.cstr");
                        builder->CreateBr(afterBB);
                        builder->SetInsertPoint(storeNullBB);
                        builder->CreateBr(afterBB);
                        builder->SetInsertPoint(afterBB);
                        llvm::PHINode* finalCstr = builder->CreatePHI(ptrTy, 2, "printf.cstr.final");
                        finalCstr->addIncoming(cstr, extractBB);
                        finalCstr->addIncoming(
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy)),
                            storeNullBB);
                        llvm::Value* destSlot = builder->CreateInBoundsGEP(
                            ptrTy, cstrArr, i, "printf.destSlot");
                        builder->CreateStore(finalCstr, destSlot);
                        llvm::Value* iNext = builder->CreateAdd(i,
                            llvm::ConstantInt::get(i64Ty, 1), "printf.i.next");
                        builder->CreateStore(iNext, iSlot);
                        builder->CreateBr(condBB);
                        builder->SetInsertPoint(doneBB);
                        return builder->CreateCall(fn, {streamArg, fmt, count, cstrArr});
                    }
                    return builder->CreateCall(fn, {streamArg, fmt, count, dataPtr});
                }
                // Unknown method or arity on a System stream; fall through to the
                // normal method-call path (which will surface a clearer error than
                // misrouting).
            }
        }

        // ----- File.<static>(...) intrinsic (cajeta.io.file Phase A) -----
        //
        // The cajeta-side `File` class in runtime/src/cajeta/io/file/File.cajeta
        // declares the static method shapes for type resolution; the bodies
        // are stubs. At each call site, MCE detects the
        // `File.<readAllBytes/writeAllBytes/openRead/openWrite>` shape and
        // emits the runtime helper call directly. Stubs never run.
        //
        // The detection looks for IdentifierExpression "File" as the
        // receiver AND verifies that "File" resolves to the
        // `cajeta.io.file.File` class (so a user-defined `class File` in
        // some other package doesn't accidentally hit this intrinsic).
        if (!children.empty()) {
            auto fileId = dynamic_pointer_cast<IdentifierExpression>(children[0]);
            if (fileId && fileId->getTextValue() == "File") {
                auto& cmap = CajetaType::getCanonicalMap();
                auto it = cmap.find("File");
                if (it == cmap.end()) {
                    it = cmap.find("cajeta.io.file.File");
                }
                CajetaClassPtr fileClass;
                if (it != cmap.end()) {
                    fileClass = std::dynamic_pointer_cast<CajetaClass>(it->second);
                }
                bool isOurFile = fileClass && fileClass->getQName()
                    && fileClass->getQName()->toCanonical()
                        == "cajeta.io.file.File";
                if (isOurFile) {
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);

                    // Common helper: load a class-String arg and unwrap to
                    // the underlying char* (skip past the CajetaArray
                    // count word). Used by every File static that takes a
                    // path. `loadStringArg` already handles this.
                    auto loadPathArg = [&](size_t idx) -> llvm::Value* {
                        return loadStringArg(module, parameters[idx].expression);
                    };

                    // Common helper: load an int8[] arg, GEP past its 8-byte
                    // count header to the raw data pointer.
                    auto loadArrayDataPtr = [&](size_t idx) -> llvm::Value* {
                        llvm::Value* arr = parameters[idx].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(arr)) {
                            arr = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        return builder->CreateInBoundsGEP(i8Ty, arr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "file.data");
                    };

                    if (methodCallName == "readAllBytes" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_read_all");
                        if (fn) {
                            llvm::Value* path = loadPathArg(0);
                            llvm::Value* arr = builder->CreateCall(fn, {path});
                            // Pin resolvedType so caller binding sees the
                            // correct CajetaArray (`int8[]`) shape — needed
                            // for the auto-drop registration on the LHS
                            // slot.
                            auto& cmap2 = CajetaType::getCanonicalMap();
                            auto it2 = cmap2.find("int8[]");
                            if (it2 != cmap2.end()) {
                                resolvedType = it2->second;
                            }
                            return arr;
                        }
                    }
                    if (methodCallName == "writeAllBytes" && parameters.size() == 3) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_write_all");
                        if (fn) {
                            llvm::Value* path = loadPathArg(0);
                            llvm::Value* data = loadArrayDataPtr(1);
                            llvm::Value* len  = parameters[2].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(len)) {
                                len = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (len && len->getType() != i32Ty
                                    && len->getType()->isIntegerTy()) {
                                len = builder->CreateIntCast(len, i32Ty, true);
                            }
                            return builder->CreateCall(fn, {path, data, len});
                        }
                    }
                    if (methodCallName == "openRead" && parameters.size() == 1) {
                        // Open the fd, then allocate + initialize a
                        // FileReader struct around it.
                        llvm::Function* openFn = module->getRuntimeFunction(
                            "__cajeta_file_open");
                        if (openFn) {
                            llvm::Value* path = loadPathArg(0);
                            // mode = 0 (OpenMode.READ ordinal).
                            llvm::Value* fd = builder->CreateCall(openFn,
                                {path, llvm::ConstantInt::get(i32Ty, 0)},
                                "file.fd");
                            CajetaClassPtr readerCls;
                            auto& cmap3 = CajetaType::getCanonicalMap();
                            auto rit = cmap3.find("FileReader");
                            if (rit == cmap3.end()) {
                                rit = cmap3.find("cajeta.io.file.FileReader");
                            }
                            if (rit != cmap3.end()) {
                                readerCls = std::dynamic_pointer_cast<CajetaClass>(rit->second);
                            }
                            if (readerCls && readerCls->getLlvmType()
                                    && llvm::isa<llvm::StructType>(readerCls->getLlvmType())) {
                                auto* structTy = llvm::cast<llvm::StructType>(
                                    readerCls->getLlvmType());
                                const llvm::DataLayout& dl =
                                    module->getLlvmModule()->getDataLayout();
                                llvm::Constant* size = llvm::ConstantInt::get(
                                    i64Ty, dl.getTypeAllocSize(structTy));
                                llvm::Value* inst = MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                                builder->CreateMemSet(inst,
                                    llvm::ConstantInt::get(i8Ty, 0),
                                    size, llvm::MaybeAlign(8));
                                // Vtable slot at field 0.
                                llvm::Constant* vtableRef =
                                    llvm::ConstantPointerNull::get(
                                        llvm::cast<llvm::PointerType>(ptrTy));
                                if (auto* vt = readerCls->getVirtualTableGlobal()) {
                                    vtableRef = CajetaModule::ensureGlobalInModule(
                                        module->getLlvmModule(), vt);
                                }
                                builder->CreateStore(vtableRef,
                                    builder->CreateStructGEP(structTy, inst, 0,
                                        "reader.vtable_slot"));
                                // fd at field 1, pos at field 2.
                                builder->CreateStore(fd,
                                    builder->CreateStructGEP(structTy, inst, 1,
                                        "reader.fd_slot"));
                                builder->CreateStore(
                                    llvm::ConstantInt::get(i64Ty, 0),
                                    builder->CreateStructGEP(structTy, inst, 2,
                                        "reader.pos_slot"));
                                resolvedType = readerCls;
                                return inst;
                            }
                        }
                    }
                    // Phase E: File.open(path, mode) — random-access
                    // handle. Same malloc+init shape as openRead/
                    // openWrite, but the resulting instance type is
                    // `cajeta.io.file.File` (which has its own
                    // vtable + fd/pos fields).
                    if ((methodCallName == "open" && parameters.size() == 2)
                            || (methodCallName == "openExclusive"
                                && parameters.size() == 1)) {
                        llvm::Function* openFn = module->getRuntimeFunction(
                            "__cajeta_file_open");
                        if (openFn) {
                            llvm::Value* path = loadPathArg(0);
                            llvm::Value* mode;
                            if (methodCallName == "openExclusive") {
                                // OpenMode.CREATE_NEW ordinal = 4.
                                mode = llvm::ConstantInt::get(i32Ty, 4);
                            } else {
                                mode = parameters[1].expression->generateCode(module);
                                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(mode)) {
                                    mode = builder->CreateLoad(a->getAllocatedType(), a);
                                }
                                if (mode && mode->getType() != i32Ty
                                        && mode->getType()->isIntegerTy()) {
                                    mode = builder->CreateIntCast(mode, i32Ty, true);
                                }
                            }
                            llvm::Value* fd = builder->CreateCall(openFn,
                                {path, mode}, "file.fd");
                            // fileClass IS the receiver — `File` class.
                            if (fileClass && fileClass->getLlvmType()
                                    && llvm::isa<llvm::StructType>(fileClass->getLlvmType())) {
                                auto* structTy = llvm::cast<llvm::StructType>(
                                    fileClass->getLlvmType());
                                const llvm::DataLayout& dl =
                                    module->getLlvmModule()->getDataLayout();
                                llvm::Constant* size = llvm::ConstantInt::get(
                                    i64Ty, dl.getTypeAllocSize(structTy));
                                llvm::Value* inst = MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                                builder->CreateMemSet(inst,
                                    llvm::ConstantInt::get(i8Ty, 0),
                                    size, llvm::MaybeAlign(8));
                                llvm::Constant* vtableRef =
                                    llvm::ConstantPointerNull::get(
                                        llvm::cast<llvm::PointerType>(ptrTy));
                                if (auto* vt = fileClass->getVirtualTableGlobal()) {
                                    vtableRef = CajetaModule::ensureGlobalInModule(
                                        module->getLlvmModule(), vt);
                                }
                                builder->CreateStore(vtableRef,
                                    builder->CreateStructGEP(structTy, inst, 0,
                                        "file.vtable_slot"));
                                builder->CreateStore(fd,
                                    builder->CreateStructGEP(structTy, inst, 1,
                                        "file.fd_slot"));
                                builder->CreateStore(
                                    llvm::ConstantInt::get(i64Ty, 0),
                                    builder->CreateStructGEP(structTy, inst, 2,
                                        "file.pos_slot"));
                                resolvedType = fileClass;
                                return inst;
                            }
                        }
                    }
                    if (methodCallName == "openWrite" && parameters.size() == 2) {
                        llvm::Function* openFn = module->getRuntimeFunction(
                            "__cajeta_file_open");
                        if (openFn) {
                            llvm::Value* path = loadPathArg(0);
                            // mode is the OpenMode enum ordinal (i32).
                            llvm::Value* mode = parameters[1].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(mode)) {
                                mode = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (mode && mode->getType() != i32Ty
                                    && mode->getType()->isIntegerTy()) {
                                mode = builder->CreateIntCast(mode, i32Ty, true);
                            }
                            llvm::Value* fd = builder->CreateCall(openFn,
                                {path, mode}, "file.fd");
                            CajetaClassPtr writerCls;
                            auto& cmap4 = CajetaType::getCanonicalMap();
                            auto wit = cmap4.find("FileWriter");
                            if (wit == cmap4.end()) {
                                wit = cmap4.find("cajeta.io.file.FileWriter");
                            }
                            if (wit != cmap4.end()) {
                                writerCls = std::dynamic_pointer_cast<CajetaClass>(wit->second);
                            }
                            if (writerCls && writerCls->getLlvmType()
                                    && llvm::isa<llvm::StructType>(writerCls->getLlvmType())) {
                                auto* structTy = llvm::cast<llvm::StructType>(
                                    writerCls->getLlvmType());
                                const llvm::DataLayout& dl =
                                    module->getLlvmModule()->getDataLayout();
                                llvm::Constant* size = llvm::ConstantInt::get(
                                    i64Ty, dl.getTypeAllocSize(structTy));
                                llvm::Value* inst = MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                                builder->CreateMemSet(inst,
                                    llvm::ConstantInt::get(i8Ty, 0),
                                    size, llvm::MaybeAlign(8));
                                llvm::Constant* vtableRef =
                                    llvm::ConstantPointerNull::get(
                                        llvm::cast<llvm::PointerType>(ptrTy));
                                if (auto* vt = writerCls->getVirtualTableGlobal()) {
                                    vtableRef = CajetaModule::ensureGlobalInModule(
                                        module->getLlvmModule(), vt);
                                }
                                builder->CreateStore(vtableRef,
                                    builder->CreateStructGEP(structTy, inst, 0,
                                        "writer.vtable_slot"));
                                builder->CreateStore(fd,
                                    builder->CreateStructGEP(structTy, inst, 1,
                                        "writer.fd_slot"));
                                builder->CreateStore(
                                    llvm::ConstantInt::get(i64Ty, 0),
                                    builder->CreateStructGEP(structTy, inst, 2,
                                        "writer.pos_slot"));
                                resolvedType = writerCls;
                                return inst;
                            }
                        }
                    }
                }
            }
        }

        // ----- TcpStream.connect / TcpListener.bind static intrinsic (NET-1.3 / NET-1.4 / b1) -----
        //
        // Mirrors the File.<static> lowering above. We detect the receiver
        // identifier ("TcpStream" / "TcpListener") resolving to the canonical
        // class, pack the SocketAddress arg into a sockaddr scratch buffer via
        // __cajeta_net_sockaddr_pack, run the socket()+connect()/bind()+listen()
        // syscall sequence, and (on success) malloc + initialize the returned
        // object exactly the way File.open allocates+returns its handle.
        //
        // SocketAddress layout: { vtable@0, ip@1 (IpAddress*), port@2 (i32) }.
        // IpAddress layout:     { vtable@0, family@1 (i32 ordinal), octets@2 (int8[]*) }.
        // The family ordinal (0=V4,1=V6) is the cajeta-portable value the pack
        // intrinsic switches on; the native socket() family is selected here
        // (AF_INET=2 everywhere; AF_INET6=23 on Windows — b1 exercises V4).
        if (!children.empty()) {
            auto netId = dynamic_pointer_cast<IdentifierExpression>(children[0]);
            std::string netName = netId ? netId->getTextValue() : "";
            bool wantStream   = (netName == "TcpStream");
            bool wantListener = (netName == "TcpListener");
            // ----- UdpSocket / socket-option intrinsic (b2) -----
            // UdpSocket.bind reuses the b1 connect/bind static path: same
            // sockaddr_pack + socket + bind sequence, but SOCK_DGRAM (no
            // listen, no reuseaddr).
            bool wantUdp      = (netName == "UdpSocket");
            if (wantStream || wantListener || wantUdp) {
                auto& cmapN = CajetaType::getCanonicalMap();
                std::string canonKey = wantStream ? "cajeta.io.net.TcpStream"
                                     : wantListener ? "cajeta.io.net.TcpListener"
                                                    : "cajeta.io.net.UdpSocket";
                auto itN = cmapN.find(netName);
                if (itN == cmapN.end()) itN = cmapN.find(canonKey);
                CajetaClassPtr netCls;
                if (itN != cmapN.end()) {
                    netCls = std::dynamic_pointer_cast<CajetaClass>(itN->second);
                }
                bool isOurNet = netCls && netCls->getQName()
                    && netCls->getQName()->toCanonical() == canonKey;
                bool isConnect = wantStream && methodCallName == "connect"
                                 && parameters.size() == 1;
                // NET-3.3 connectAsync: same sockaddr_pack + socket prologue as
                // the blocking connect, but the socket is made non-blocking and
                // an in-progress connect parks the fiber on the reactor's
                // writable readiness, then reads SO_ERROR — a distinct control
                // shape lowered below.
                bool isConnectAsync = wantStream && methodCallName == "connectAsync"
                                 && parameters.size() == 1;
                bool isBind    = wantListener && methodCallName == "bind"
                                 && parameters.size() == 1;
                bool isUdpBind = wantUdp && methodCallName == "bind"
                                 && parameters.size() == 1;
                if (isOurNet && (isConnect || isConnectAsync || isBind || isUdpBind)) {
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);

                    llvm::Function* sockFn = module->getRuntimeFunction("__cajeta_net_socket");
                    llvm::Function* packFn = module->getRuntimeFunction("__cajeta_net_sockaddr_pack");
                    llvm::Function* connFn = module->getRuntimeFunction("__cajeta_net_connect");
                    llvm::Function* bindFn = module->getRuntimeFunction("__cajeta_net_bind");
                    llvm::Function* listenFn = module->getRuntimeFunction("__cajeta_net_listen");
                    llvm::Function* reuseFn = module->getRuntimeFunction("__cajeta_net_set_reuseaddr");
                    llvm::Function* closeFn = module->getRuntimeFunction("__cajeta_net_close");
                    llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");
                    // connectAsync (NET-3.3) intrinsics: toggle non-blocking,
                    // classify the in-progress connect, park on writability,
                    // and read the SO_ERROR outcome.
                    llvm::Function* setNbFn   = module->getRuntimeFunction("__cajeta_net_set_nonblocking");
                    llvm::Function* inProgFn  = module->getRuntimeFunction("__cajeta_net_is_in_progress");
                    llvm::Function* awaitWrFn = module->getRuntimeFunction("__cajeta_net_await_writable");
                    llvm::Function* connResFn = module->getRuntimeFunction("__cajeta_net_connect_result");

                    // Resolve the SocketAddress / IpAddress llvm struct types
                    // so we can GEP the ip / family / octets / port fields.
                    CajetaClassPtr saCls;
                    CajetaClassPtr ipCls;
                    {
                        auto sIt = cmapN.find("cajeta.io.net.SocketAddress");
                        if (sIt == cmapN.end()) sIt = cmapN.find("SocketAddress");
                        if (sIt != cmapN.end())
                            saCls = std::dynamic_pointer_cast<CajetaClass>(sIt->second);
                        auto iIt = cmapN.find("cajeta.io.net.IpAddress");
                        if (iIt == cmapN.end()) iIt = cmapN.find("IpAddress");
                        if (iIt != cmapN.end())
                            ipCls = std::dynamic_pointer_cast<CajetaClass>(iIt->second);
                    }

                    if (sockFn && packFn
                            && (isConnect ? (connFn != nullptr)
                                : isConnectAsync ? (connFn && setNbFn && inProgFn
                                                    && awaitWrFn && connResFn)
                                : isUdpBind ? (bindFn != nullptr)
                                            : (bindFn && listenFn))
                            && saCls && ipCls
                            && llvm::isa<llvm::StructType>(saCls->getLlvmType())
                            && llvm::isa<llvm::StructType>(ipCls->getLlvmType())) {
                        auto* saTy = llvm::cast<llvm::StructType>(saCls->getLlvmType());
                        auto* ipTy = llvm::cast<llvm::StructType>(ipCls->getLlvmType());

                        // Generate the SocketAddress arg pointer.
                        llvm::Value* sa = parameters[0].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(sa)) {
                            sa = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        // ip = sa.ip (field 1); port = sa.port (field 2).
                        llvm::Value* ipSlot = builder->CreateStructGEP(saTy, sa, 1, "na.ip_slot");
                        llvm::Value* ip = builder->CreateLoad(ptrTy, ipSlot, "na.ip");
                        llvm::Value* portSlot = builder->CreateStructGEP(saTy, sa, 2, "na.port_slot");
                        llvm::Value* port = builder->CreateLoad(i32Ty, portSlot, "na.port");
                        // family = ip.family (field 1, i32 ordinal); octets = ip.octets (field 2).
                        llvm::Value* famSlot = builder->CreateStructGEP(ipTy, ip, 1, "na.fam_slot");
                        llvm::Value* family = builder->CreateLoad(i32Ty, famSlot, "na.family");
                        llvm::Value* octSlot = builder->CreateStructGEP(ipTy, ip, 2, "na.oct_slot");
                        llvm::Value* octArr = builder->CreateLoad(ptrTy, octSlot, "na.octets_arr");
                        llvm::Value* octData = builder->CreateInBoundsGEP(
                            i8Ty, octArr, llvm::ConstantInt::get(i64Ty, 8), "na.octets");

                        // Pack into a sockaddr scratch buffer.
                        llvm::Value* scratch = builder->CreateAlloca(
                            llvm::ArrayType::get(i8Ty, 128), nullptr, "na.scratch");
                        llvm::Value* addrlen = builder->CreateCall(packFn,
                            {family, octData, port, scratch,
                             llvm::ConstantInt::get(i32Ty, 128)}, "na.addrlen");

                        // Native socket() family: AF_INET(2) for V4, AF_INET6
                        // (23 on Windows) for V6. SOCK_STREAM(1), proto 0.
                        llvm::Value* isV4 = builder->CreateICmpEQ(family,
                            llvm::ConstantInt::get(i32Ty, 0), "na.isV4");
                        llvm::Value* nativeFamily = builder->CreateSelect(isV4,
                            llvm::ConstantInt::get(i32Ty, 2),
                            llvm::ConstantInt::get(i32Ty, 23), "na.nativeFamily");
                        // SOCK_STREAM(1) for TCP, SOCK_DGRAM(2) for UDP — the
                        // portable values __cajeta_net_socket maps. (b2)
                        int32_t sockType = isUdpBind ? 2 : 1;
                        llvm::Value* fd = builder->CreateCall(sockFn,
                            {nativeFamily, llvm::ConstantInt::get(i32Ty, sockType),
                             llvm::ConstantInt::get(i32Ty, 0)}, "na.fd");

                        // Failure-sentinel check helper: if `cond` is true, throw
                        // an informational tag and unreachable.
                        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
                        auto throwIf = [&](llvm::Value* cond, uint64_t tag,
                                           llvm::Value* fdToClose) {
                            llvm::BasicBlock* failBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.fail", parentFn);
                            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.ok", parentFn);
                            builder->CreateCondBr(cond, failBB, okBB);
                            builder->SetInsertPoint(failBB);
                            if (fdToClose && closeFn) {
                                builder->CreateCall(closeFn, {fdToClose});
                            }
                            if (throwFn) {
                                llvm::Value* tagPtr = builder->CreateIntToPtr(
                                    llvm::ConstantInt::get(i64Ty, tag), ptrTy);
                                builder->CreateCall(throwFn, {tagPtr});
                            }
                            builder->CreateUnreachable();
                            builder->SetInsertPoint(okBB);
                        };

                        // sockaddr_pack failure: addrlen <= 0.
                        throwIf(builder->CreateICmpSLE(addrlen,
                            llvm::ConstantInt::get(i32Ty, 0)), 0x100, nullptr);

                        // socket() failure: fd < 0.
                        // NOTE (b1): the failure path throws a small integer
                        // sentinel (< the zero-page boundary __cajeta_throw
                        // guards on), so an uncaught socket failure exits
                        // cleanly (exit 1) rather than dereferencing a bogus
                        // pointer. Constructing a proper NetException subtype
                        // (so callers can `catch (NetException)`) is a later
                        // increment; b1's success path is what the loopback
                        // echo exercises.
                        throwIf(builder->CreateICmpSLT(fd,
                            llvm::ConstantInt::get(i32Ty, 0)), 0x101, nullptr);

                        if (isConnect) {
                            llvm::Value* rc = builder->CreateCall(connFn,
                                {fd, scratch, addrlen}, "na.connect_rc");
                            // connect failure: rc != 0 → close fd then throw.
                            throwIf(builder->CreateICmpNE(rc,
                                llvm::ConstantInt::get(i32Ty, 0)), 0x102, fd);
                        } else if (isConnectAsync) {
                            // ----- non-blocking connect (NET-3.3 connectAsync) -----
                            // Mirrors the C dance documented above
                            // __cajeta_net_connect: make the socket non-blocking,
                            // issue connect; rc==0 means it completed immediately
                            // (loopback usually does). Otherwise distinguish the
                            // in-progress case (EINPROGRESS / WSAEWOULDBLOCK) — for
                            // which we park the fiber on reactor writability then
                            // read SO_ERROR — from a hard connect failure.
                            // is_in_progress() reads the errno set by connect, so
                            // nothing may syscall between the two calls (only the
                            // icmp/branch below).
                            builder->CreateCall(setNbFn,
                                {fd, llvm::ConstantInt::get(i32Ty, 1)});
                            llvm::Value* rc = builder->CreateCall(connFn,
                                {fd, scratch, addrlen}, "na.aconnect_rc");
                            llvm::Value* immediate = builder->CreateICmpEQ(rc,
                                llvm::ConstantInt::get(i32Ty, 0), "na.aconnect_now");
                            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_done", parentFn);
                            llvm::BasicBlock* pendingBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_pending", parentFn);
                            builder->CreateCondBr(immediate, doneBB, pendingBB);

                            // pending: connect returned -1; is it in-flight or a
                            // hard error?
                            builder->SetInsertPoint(pendingBB);
                            llvm::Value* inProg = builder->CreateCall(inProgFn,
                                {}, "na.aconnect_inprog");
                            llvm::Value* isInProg = builder->CreateICmpNE(inProg,
                                llvm::ConstantInt::get(i32Ty, 0), "na.aconnect_isinprog");
                            llvm::BasicBlock* awaitBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_await", parentFn);
                            llvm::BasicBlock* hardFailBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_hardfail", parentFn);
                            builder->CreateCondBr(isInProg, awaitBB, hardFailBB);

                            // hard connect failure (e.g. ECONNREFUSED returned
                            // synchronously): close + throw.
                            builder->SetInsertPoint(hardFailBB);
                            if (closeFn) builder->CreateCall(closeFn, {fd});
                            if (throwFn) {
                                llvm::Value* tagPtr = builder->CreateIntToPtr(
                                    llvm::ConstantInt::get(i64Ty, 0x106), ptrTy);
                                builder->CreateCall(throwFn, {tagPtr});
                            }
                            builder->CreateUnreachable();

                            // in-progress: park the fiber until the socket is
                            // writable, then read SO_ERROR to learn the outcome.
                            builder->SetInsertPoint(awaitBB);
                            builder->CreateCall(awaitWrFn, {fd});
                            llvm::Value* soerr = builder->CreateCall(connResFn,
                                {fd}, "na.aconnect_soerr");
                            llvm::Value* soOk = builder->CreateICmpEQ(soerr,
                                llvm::ConstantInt::get(i32Ty, 0), "na.aconnect_sook");
                            llvm::BasicBlock* soFailBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_sofail", parentFn);
                            builder->CreateCondBr(soOk, doneBB, soFailBB);

                            // SO_ERROR != 0 → the connect failed after readiness.
                            builder->SetInsertPoint(soFailBB);
                            if (closeFn) builder->CreateCall(closeFn, {fd});
                            if (throwFn) {
                                llvm::Value* tagPtr = builder->CreateIntToPtr(
                                    llvm::ConstantInt::get(i64Ty, 0x107), ptrTy);
                                builder->CreateCall(throwFn, {tagPtr});
                            }
                            builder->CreateUnreachable();

                            // converge: the socket is connected (non-blocking).
                            // Fall through to the shared wrap code below.
                            builder->SetInsertPoint(doneBB);
                        } else if (isUdpBind) {
                            // ----- UdpSocket.bind (b2): bind only, no listen.
                            // SO_REUSEADDR best-effort (multicast group sharing).
                            if (reuseFn) {
                                builder->CreateCall(reuseFn,
                                    {fd, llvm::ConstantInt::get(i32Ty, 1)});
                            }
                            llvm::Value* brc = builder->CreateCall(bindFn,
                                {fd, scratch, addrlen}, "na.udp_bind_rc");
                            throwIf(builder->CreateICmpNE(brc,
                                llvm::ConstantInt::get(i32Ty, 0)), 0x105, fd);
                        } else {
                            // bind: set SO_REUSEADDR (best-effort), bind, listen.
                            if (reuseFn) {
                                builder->CreateCall(reuseFn,
                                    {fd, llvm::ConstantInt::get(i32Ty, 1)});
                            }
                            llvm::Value* brc = builder->CreateCall(bindFn,
                                {fd, scratch, addrlen}, "na.bind_rc");
                            throwIf(builder->CreateICmpNE(brc,
                                llvm::ConstantInt::get(i32Ty, 0)), 0x103, fd);
                            llvm::Value* lrc = builder->CreateCall(listenFn,
                                {fd, llvm::ConstantInt::get(i32Ty, 128)}, "na.listen_rc");
                            throwIf(builder->CreateICmpNE(lrc,
                                llvm::ConstantInt::get(i32Ty, 0)), 0x104, fd);
                        }

                        // Allocate + init the returned TcpStream / TcpListener
                        // wrapping `fd` (mirror File.open's malloc+memset+vtable+fd).
                        if (netCls->getLlvmType()
                                && llvm::isa<llvm::StructType>(netCls->getLlvmType())) {
                            auto* outTy = llvm::cast<llvm::StructType>(netCls->getLlvmType());
                            const llvm::DataLayout& dl =
                                module->getLlvmModule()->getDataLayout();
                            llvm::Constant* size = llvm::ConstantInt::get(
                                i64Ty, dl.getTypeAllocSize(outTy));
                            llvm::Value* inst = MemoryManager::createMallocInstruction(
                                module, size, builder->GetInsertBlock());
                            builder->CreateMemSet(inst,
                                llvm::ConstantInt::get(i8Ty, 0),
                                size, llvm::MaybeAlign(8));
                            llvm::Constant* vtableRef =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            if (auto* vt = netCls->getVirtualTableGlobal()) {
                                vtableRef = CajetaModule::ensureGlobalInModule(
                                    module->getLlvmModule(), vt);
                            }
                            builder->CreateStore(vtableRef,
                                builder->CreateStructGEP(outTy, inst, 0, "na.vtable_slot"));
                            // fd at field index 1.
                            builder->CreateStore(fd,
                                builder->CreateStructGEP(outTy, inst, 1, "na.fd_slot"));
                            resolvedType = netCls;
                            return inst;
                        }
                    }
                }
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
                        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                            lm, llvm::Intrinsic::fabs, {x->getType()});
                        return builder->CreateCall(fn, {x});
                    }
                    x = toI64(x);
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
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
                        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(lm, id, {f64Ty});
                        return builder->CreateCall(fn, {a, b});
                    }
                    a = toI64(a);
                    b = toI64(b);
                    llvm::Intrinsic::ID id = methodCallName == "max"
                        ? llvm::Intrinsic::smax : llvm::Intrinsic::smin;
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(lm, id, {i64Ty});
                    return builder->CreateCall(fn, {a, b});
                }
                if (methodCallName == "sqrt" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::sqrt, {f64Ty});
                    return builder->CreateCall(fn, {x});
                }
                if (methodCallName == "pow" && parameters.size() == 2) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Value* y = toF64(loadArg(1));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::pow, {f64Ty});
                    return builder->CreateCall(fn, {x, y});
                }
                if (methodCallName == "floor" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::floor, {f64Ty});
                    return builder->CreateCall(fn, {x});
                }
                if (methodCallName == "ceil" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::ceil, {f64Ty});
                    return builder->CreateCall(fn, {x});
                }
                if (methodCallName == "round" && parameters.size() == 1) {
                    // Match Java's Math.round(double) → long: half-up rounding to i64.
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
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
                        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(lm, u.id, {f64Ty});
                        return builder->CreateCall(fn, {x});
                    }
                }
                // tan has no direct intrinsic in LLVM 18 — emit sin/cos division.
                if (methodCallName == "tan" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* sinFn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::sin, {f64Ty});
                    llvm::Function* cosFn = llvm::Intrinsic::getOrInsertDeclaration(
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
                // Source-tagged drop-chain entry diagnostics (CompilerModes.md
                // § Source-tagged drop-chain entries). Reads the head entry's
                // alloc-site tags as recorded by __cajeta_drop_push_debug.
                // Returns 0 / null when sourceTags is off or the chain is
                // empty. Test-only intrinsics; the production diagnostic
                // surface is the SIGABRT handler (P4.2).
                if (ns == "Cajeta" && methodCallName == "dropChainHeadAllocLine" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_drop_chain_head_alloc_line");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "dropChainHeadAllocFile" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_drop_chain_head_alloc_file");
                    return builder->CreateCall(fn, {});
                }
                // Walk the chain and print every entry to stderr; returns
                // the count printed. Exposed both for the SIGABRT handler
                // (which calls the runtime helper directly) and for tests
                // that want to verify the dump shape without aborting.
                if (ns == "Cajeta" && methodCallName == "dumpDropChain" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_dump_drop_chain");
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
                // docs/specification/concurrent/Concurrency.md § Synchronization primitives.
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
                // FiberLocal intrinsics (docs/specification/concurrent/FiberLocal.md). Ambient
                // per-request state, fiber-keyed and scope-restored. The key is a
                // FiberLocal<T> object identity; values are reference-typed (a
                // ptr), so the surface restricts T to a reference type in v1. All
                // pass through as opaque ptr (object<->pointer convert freely).
                // push/get/capture/install return a ptr; pop/free return void.
                if (ns == "Cajeta" && methodCallName == "fiberLocalPush" && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_local_push");
                    return builder->CreateCall(fn, {loadValue(0), loadValue(1)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberLocalPop" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_local_pop");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberLocalGet" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_local_get");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberLocalIsBound" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_local_is_bound");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberContextCapture" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_context_capture");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "fiberContextInstall" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_context_install");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberContextFree" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_context_free");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                // Low-level memory/bit intrinsics for SWAR-class code
                // (cajeta.io.Buffer). Pure codegen — no runtime C function.
                // loadU64(int8[] buf, int64 off): unaligned little-endian 64-bit
                // load of the array's bytes at byte offset `off`. The array data
                // lives at header+8 (the count word precedes it — same ABI the
                // @Native bridge uses). Caller must keep off in [0, count-8].
                if (ns == "Cajeta" && methodCallName == "loadU64" && parameters.size() == 2) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* i64Ty = builder->getInt64Ty();
                    llvm::Value* hdr = loadValue(0);   // array header pointer
                    llvm::Value* off = loadValue(1);   // i64 byte offset
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* eltPtr = builder->CreateGEP(
                        i8Ty, data, off, "buf_word_ptr");
                    llvm::LoadInst* ld = builder->CreateLoad(i64Ty, eltPtr, "buf_word");
                    ld->setAlignment(llvm::Align(1));
                    return ld;
                }
                // storeU64(int8[] buf, int64 off, int64 val): the write dual of
                // loadU64 — unaligned little-endian 64-bit store at byte offset
                // `off`. Used to materialize i64 constant tables (e.g. XXH3's
                // 192-byte secret) into a byte buffer. Caller keeps off in
                // [0, count-8].
                if (ns == "Cajeta" && methodCallName == "storeU64" && parameters.size() == 3) {
                    auto* i8Ty = builder->getInt8Ty();
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* off = loadValue(1);
                    llvm::Value* val = loadValue(2);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* eltPtr = builder->CreateGEP(
                        i8Ty, data, off, "buf_word_ptr");
                    llvm::StoreInst* st = builder->CreateStore(val, eltPtr);
                    st->setAlignment(llvm::Align(1));
                    return st;
                }
                // hashBytes(int8[] buf, int64 len) -> int64: XXH3-64 (seeded) over
                // the first `len` bytes of the array. Bridges int8[] -> uint8_t*
                // (data at header+8, same ABI as loadU64) and calls the runtime
                // __cajeta_hash_bytes. Backs String.hash(); len<=0 is the empty-
                // input hash (the runtime clamps negative len to 0).
                if (ns == "Cajeta" && methodCallName == "hashBytes" && parameters.size() == 2) {
                    auto* i8Ty = builder->getInt8Ty();
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* len = loadValue(1);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "hash_data");
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_hash_bytes");
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateCall(fn, {data, len});
                }
                // allocBytes(int64 n) -> int8[]: allocate an owned int8[] of n
                // elements with the data region LEFT UNINITIALIZED (no calloc
                // zeroing). For buffers fully overwritten before any read
                // (StringBuilder grow/toString, concat payloads); the array is
                // live-set tracked and drops/frees exactly like a zeroed one.
                if (ns == "Cajeta" && methodCallName == "allocBytes" && parameters.size() == 1) {
                    auto* i64Ty = builder->getInt64Ty();
                    llvm::Value* count = loadValue(0);
                    if (count->getType() != i64Ty) {
                        count = builder->CreateIntCast(count, i64Ty, /*isSigned=*/true);
                    }
                    CajetaTypePtr i8 = CajetaType::of("int8");
                    auto arrTy = std::make_shared<CajetaArray>(module, i8);
                    module->getStructures()[arrTy->toCanonical()] =
                        std::static_pointer_cast<CajetaClass>(arrTy);
                    const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
                    llvm::Value* headerSize = builder->getInt64(
                        dl.getTypeAllocSize(arrTy->getLlvmType()));
                    llvm::Value* elemSize = builder->getInt64(
                        dl.getTypeAllocSize(arrTy->getElementLlvmType(&llvmCtx)));
                    llvm::Function* allocFn =
                        module->getRuntimeFunction("__cajeta_new_array_header_uninit");
                    resolvedType = arrTy;
                    return builder->CreateCall(allocFn, {headerSize, elemSize, count});
                }
                // moveMask() -> int64: the caller-side ownership-transfer mask for
                // THIS call (bit i set iff user-arg i was passed `#x`). Read once at
                // method entry; the compiler sets the TLS before a `#`-bearing call
                // and clears it after (see the call-site emission). Lets a plain-`T`
                // param method (e.g. HashMap.put) learn which args it owns.
                if (ns == "Cajeta" && methodCallName == "moveMask" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_move_mask_get");
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateCall(fn, {});
                }
                // f32ToBits(float32 x) -> int32: reinterpret a float's IEEE-754
                // bits as a 32-bit integer (LLVM bitcast — NOT a value
                // conversion; `(int32) x` would truncate the numeric value).
                // The inverse of bitsToF32. Enables binary float serialization
                // (little-endian float32 .npy) that the value-cast cannot express.
                if (ns == "Cajeta" && methodCallName == "f32ToBits" && parameters.size() == 1) {
                    llvm::Value* x = loadValue(0);
                    llvm::Value* b = builder->CreateBitCast(x, i32Ty, "f32_bits");
                    resolvedType = CajetaType::of("int32");
                    return b;
                }
                // bitsToF32(int32 b) -> float32: reinterpret 32 integer bits as an
                // IEEE-754 float (LLVM bitcast). Inverse of f32ToBits; the float32
                // .npy reader uses it.
                if (ns == "Cajeta" && methodCallName == "bitsToF32" && parameters.size() == 1) {
                    auto* f32Ty = llvm::Type::getFloatTy(llvmCtx);
                    llvm::Value* b = loadValue(0);
                    llvm::Value* x = builder->CreateBitCast(b, f32Ty, "bits_f32");
                    resolvedType = CajetaType::of("float32");
                    return x;
                }
                // f64ToBits(float64 x) -> int64: reinterpret a double's IEEE-754
                // bits as a 64-bit integer (LLVM bitcast). Inverse of bitsToF64.
                if (ns == "Cajeta" && methodCallName == "f64ToBits" && parameters.size() == 1) {
                    llvm::Value* x = loadValue(0);
                    llvm::Value* b = builder->CreateBitCast(x, i64Ty, "f64_bits");
                    resolvedType = CajetaType::of("int64");
                    return b;
                }
                // bitsToF64(int64 b) -> float64: reinterpret 64 integer bits as a
                // double (LLVM bitcast). Inverse of f64ToBits.
                if (ns == "Cajeta" && methodCallName == "bitsToF64" && parameters.size() == 1) {
                    llvm::Value* b = loadValue(0);
                    llvm::Value* x = builder->CreateBitCast(b, f64Ty, "bits_f64");
                    resolvedType = CajetaType::of("float64");
                    return x;
                }
                // ctz64(int64 x): count trailing zero bits, 0..64 (x==0 -> 64).
                // Maps to @llvm.cttz.i64; result truncated to int32.
                if (ns == "Cajeta" && methodCallName == "ctz64" && parameters.size() == 1) {
                    auto* lmod = module->getLlvmModule();
                    auto* i64Ty = builder->getInt64Ty();
                    llvm::Value* x = loadValue(0);
                    llvm::Function* cttz = llvm::Intrinsic::getOrInsertDeclaration(
                        lmod, llvm::Intrinsic::cttz, {i64Ty});
                    llvm::Value* r = builder->CreateCall(
                        cttz, {x, builder->getFalse()}, "ctz");
                    return builder->CreateTrunc(r, builder->getInt32Ty(), "ctz32");
                }
                // vload16(int8[] buf, int64 off) -> Vector<int8,16>: load a
                // 16-byte block (unaligned) from the array data at byte offset
                // `off`. Bind to a Vector<int8,16> local. The SIMD scanner's
                // block source; pairs with v.eqMask / tableLookup.
                if (ns == "Cajeta" && methodCallName == "vload16" && parameters.size() == 2) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* v16  = llvm::FixedVectorType::get(i8Ty, 16);
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* off = loadValue(1);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* ptr = builder->CreateGEP(
                        i8Ty, data, off, "buf_blk_ptr");
                    llvm::LoadInst* ld = builder->CreateLoad(v16, ptr, "vload16");
                    ld->setAlignment(llvm::Align(1));
                    return ld;
                }
                // --- Wide SIMD primitives (Vector<int64,8> = 512-bit / AVX-512) --
                // The XXH3 bulk-hash path lives on these: a 64-byte stripe is 8
                // i64 lanes, the accumulate is `acc += swapPairs(data); acc +=
                // (key&0xffffffff)*(key>>32)`, and the final merge reads lanes.
                // vload8i64(int8[] buf, int64 off) -> Vector<int64,8>: unaligned
                // 64-byte load as 8 i64 lanes (one stripe / one secret window).
                if (ns == "Cajeta" && methodCallName == "vload8i64" && parameters.size() == 2) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* v8 = llvm::FixedVectorType::get(builder->getInt64Ty(), 8);
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* off = loadValue(1);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* ptr = builder->CreateGEP(i8Ty, data, off, "v8_blk_ptr");
                    llvm::LoadInst* ld = builder->CreateLoad(v8, ptr, "vload8i64");
                    ld->setAlignment(llvm::Align(1));
                    resolvedType = CajetaVector::validateAndCreate(
                        module, CajetaType::of("int64"), 8);
                    return ld;
                }
                // vstore8i64(Vector<int64,8> v, int8[] buf, int64 off): the dual of
                // vload8i64 — unaligned 64-byte store of 8 i64 lanes (used to
                // materialize the seeded secret + the accumulators for merge).
                if (ns == "Cajeta" && methodCallName == "vstore8i64" && parameters.size() == 3) {
                    auto* i8Ty = builder->getInt8Ty();
                    llvm::Value* vec = loadValue(0);
                    llvm::Value* hdr = loadValue(1);
                    llvm::Value* off = loadValue(2);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* ptr = builder->CreateGEP(i8Ty, data, off, "v8_st_ptr");
                    llvm::StoreInst* st = builder->CreateStore(vec, ptr);
                    st->setAlignment(llvm::Align(1));
                    return st;
                }
                // --- float64 SIMD (Vector<float64,8> = 512-bit / AVX-512) ----
                // The numeric-kernel path (matmul / dot-product) lives on these,
                // the float64 analogue of vload8i64. Unlike vload8i64 (int8[] byte
                // buffer, BYTE offset), these take a float64[] and an ELEMENT index
                // -- the array header is `{ i64 size, [0 x double] data }`, so the
                // data starts 8 bytes in and element idx is a double-stride GEP.
                // simd-numeric-kernels-spec.md §2.
                // vload8f64(float64[] arr, int32 idx) -> Vector<float64,8>.
                if (ns == "Cajeta" && methodCallName == "vload8f64" && parameters.size() == 2) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* dblTy = builder->getDoubleTy();
                    auto* v8 = llvm::FixedVectorType::get(dblTy, 8);
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* idx = loadValue(1);
                    idx = builder->CreateIntCast(idx, builder->getInt64Ty(), /*isSigned=*/true);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "f64_data");
                    llvm::Value* ptr = builder->CreateGEP(dblTy, data, idx, "v8f64_ptr");
                    llvm::LoadInst* ld = builder->CreateLoad(v8, ptr, "vload8f64");
                    ld->setAlignment(llvm::Align(1));
                    resolvedType = CajetaVector::validateAndCreate(
                        module, CajetaType::of("float64"), 8);
                    return ld;
                }
                // vstore8f64(Vector<float64,8> v, float64[] arr, int32 idx).
                if (ns == "Cajeta" && methodCallName == "vstore8f64" && parameters.size() == 3) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* dblTy = builder->getDoubleTy();
                    llvm::Value* vec = loadValue(0);
                    llvm::Value* hdr = loadValue(1);
                    llvm::Value* idx = loadValue(2);
                    idx = builder->CreateIntCast(idx, builder->getInt64Ty(), /*isSigned=*/true);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "f64_data");
                    llvm::Value* ptr = builder->CreateGEP(dblTy, data, idx, "v8f64_st_ptr");
                    llvm::StoreInst* st = builder->CreateStore(vec, ptr);
                    st->setAlignment(llvm::Align(1));
                    return st;
                }
                // vsum8f64(Vector<float64,8> v) -> float64: horizontal sum. Uses a
                // fast (reassociating) tree reduction -- the reduction order is not
                // significant for these kernels (the dot-product check tolerates FP
                // reassociation; matmul never reduces a vector).
                if (ns == "Cajeta" && methodCallName == "vsum8f64" && parameters.size() == 1) {
                    llvm::Value* vec = loadValue(0);
                    llvm::Value* acc0 = llvm::ConstantFP::get(builder->getDoubleTy(), 0.0);
                    llvm::Value* red = builder->CreateFAddReduce(acc0, vec);
                    if (auto* inst = llvm::dyn_cast<llvm::Instruction>(red)) {
                        llvm::FastMathFlags fmf;
                        fmf.setFast();
                        inst->setFastMathFlags(fmf);
                    }
                    resolvedType = CajetaType::of("float64");
                    return red;
                }
                // vswapPairs(Vector<int64,8> v) -> Vector<int64,8>: swap adjacent
                // lanes (0<->1, 2<->3, 4<->5, 6<->7) — XXH3's `acc[lane^1]` swap.
                if (ns == "Cajeta" && methodCallName == "vswapPairs" && parameters.size() == 1) {
                    llvm::Value* vec = loadValue(0);
                    int maskArr[8] = {1, 0, 3, 2, 5, 4, 7, 6};
                    llvm::SmallVector<int, 8> mask(maskArr, maskArr + 8);
                    llvm::Value* sw = builder->CreateShuffleVector(vec, vec, mask, "vswapPairs");
                    resolvedType = CajetaVector::validateAndCreate(
                        module, CajetaType::of("int64"), 8);
                    return sw;
                }
                // vlane(Vector<int64,8> v, int32 i) -> int64: extract lane i
                // (the final mergeAccs reads the eight accumulators pairwise).
                if (ns == "Cajeta" && methodCallName == "vlane" && parameters.size() == 2) {
                    llvm::Value* vec = loadValue(0);
                    llvm::Value* idx = loadValue(1);
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateExtractElement(vec, idx, "vlane");
                }
                // popcount64(int64 x) -> int32: set-bit count via @llvm.ctpop.i64.
                // Counts bits in a SIMD mask (e.g. structural/quote/scalar masks).
                if (ns == "Cajeta" && methodCallName == "popcount64" && parameters.size() == 1) {
                    auto* lmod = module->getLlvmModule();
                    auto* i64Ty = builder->getInt64Ty();
                    llvm::Value* x = loadValue(0);
                    llvm::Function* ctpop = llvm::Intrinsic::getOrInsertDeclaration(
                        lmod, llvm::Intrinsic::ctpop, {i64Ty});
                    llvm::Value* r = builder->CreateCall(ctpop, {x}, "popcnt");
                    return builder->CreateTrunc(r, builder->getInt32Ty(), "popcnt32");
                }
                // Condition-variable intrinsics (R7-B). Fiber-aware, paired
                // with a lock handle; `Mutex<T>.withLockWhen` builds on them.
                // condvarWait(cv, lock) atomically releases `lock`, parks the
                // fiber (or cond_waits on the main thread), and reacquires
                // `lock` on wake. condvarNotifyAll wakes every waiter (which
                // re-checks its own predicate). See docs/specification/concurrent/Concurrency.md.
                if (ns == "Cajeta" && methodCallName == "condvarNew" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_condvar_new");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "condvarWait" && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_condvar_wait");
                    llvm::Value* cv = loadValue(0);
                    llvm::Value* lock = loadValue(1);
                    return builder->CreateCall(fn, {cv, lock});
                }
                if (ns == "Cajeta" && methodCallName == "condvarNotifyAll" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_condvar_notify_all");
                    llvm::Value* cv = loadValue(0);
                    return builder->CreateCall(fn, {cv});
                }
                if (ns == "Cajeta" && methodCallName == "condvarDestroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_condvar_destroy");
                    llvm::Value* cv = loadValue(0);
                    return builder->CreateCall(fn, {cv});
                }
                // Reader-writer lock intrinsics (R7-D). Fiber-aware; back
                // `RwLock<T>`. Many readers share; a writer is exclusive.
                if (ns == "Cajeta" && methodCallName == "rwlockNew" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_new");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockRdlock" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_rdlock");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockWrlock" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_wrlock");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockRdunlock" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_rdunlock");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockWrunlock" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_wrunlock");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockDestroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_destroy");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                // Atomic<T> intrinsics (R8 Slice 1). The new/destroy pair is
                // a small runtime helper (malloc/free of the underlying
                // word). The load/store/fetch_add/compareAndSet are emitted
                // INLINE as LLVM atomic instructions — a runtime-call would
                // defeat atomicity's whole purpose and block the optimizer
                // from reasoning about ordering. All seq_cst for v1;
                // memory-order parameterization is R8 Slice 1b.
                // See docs/specification/concurrent/Concurrency.md and the AtomicInt32 /
                // AtomicInt64 wrapper classes in cajeta.concurrent.
                if (ns == "Cajeta" && methodCallName == "atomicI32New" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_atomic_i32_new");
                    llvm::Value* v = loadValue(0);
                    if (v->getType() != i32Ty) v = builder->CreateIntCast(v, i32Ty, /*isSigned=*/true);
                    return builder->CreateCall(fn, {v});
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32Destroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_atomic_i32_destroy");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32Load" && parameters.size() == 1) {
                    auto* load = builder->CreateLoad(i32Ty, loadValue(0), "atomic.i32.load");
                    load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                    load->setAlignment(llvm::Align(4));
                    return load;
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32Store" && parameters.size() == 2) {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != i32Ty) v = builder->CreateIntCast(v, i32Ty, /*isSigned=*/true);
                    auto* store = builder->CreateStore(v, loadValue(0));
                    store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                    store->setAlignment(llvm::Align(4));
                    return store;
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32FetchAdd" && parameters.size() == 2) {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != i32Ty) v = builder->CreateIntCast(v, i32Ty, /*isSigned=*/true);
                    return builder->CreateAtomicRMW(
                        llvm::AtomicRMWInst::Add, loadValue(0), v,
                        llvm::MaybeAlign(4),
                        llvm::AtomicOrdering::SequentiallyConsistent);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32CompareAndSet"
                        && parameters.size() == 3) {
                    llvm::Value* expected = loadValue(1);
                    llvm::Value* desired = loadValue(2);
                    if (expected->getType() != i32Ty) expected = builder->CreateIntCast(expected, i32Ty, true);
                    if (desired->getType() != i32Ty) desired = builder->CreateIntCast(desired, i32Ty, true);
                    auto* cmpxchg = builder->CreateAtomicCmpXchg(
                        loadValue(0), expected, desired,
                        llvm::MaybeAlign(4),
                        llvm::AtomicOrdering::SequentiallyConsistent,
                        llvm::AtomicOrdering::SequentiallyConsistent);
                    // The cmpxchg result is `{ T old, i1 success }`; we
                    // surface the boolean success flag (matches Java's
                    // compareAndSet shape).
                    return builder->CreateExtractValue(cmpxchg, 1, "atomic.cas.ok");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64New" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_atomic_i64_new");
                    llvm::Value* v = loadValue(0);
                    if (v->getType() != i64Ty) v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                    return builder->CreateCall(fn, {v});
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64Destroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_atomic_i64_destroy");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64Load" && parameters.size() == 1) {
                    auto* load = builder->CreateLoad(i64Ty, loadValue(0), "atomic.i64.load");
                    load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                    load->setAlignment(llvm::Align(8));
                    return load;
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64Store" && parameters.size() == 2) {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != i64Ty) v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                    auto* store = builder->CreateStore(v, loadValue(0));
                    store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                    store->setAlignment(llvm::Align(8));
                    return store;
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64FetchAdd" && parameters.size() == 2) {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != i64Ty) v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                    return builder->CreateAtomicRMW(
                        llvm::AtomicRMWInst::Add, loadValue(0), v,
                        llvm::MaybeAlign(8),
                        llvm::AtomicOrdering::SequentiallyConsistent);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64CompareAndSet"
                        && parameters.size() == 3) {
                    llvm::Value* expected = loadValue(1);
                    llvm::Value* desired = loadValue(2);
                    if (expected->getType() != i64Ty) expected = builder->CreateIntCast(expected, i64Ty, true);
                    if (desired->getType() != i64Ty) desired = builder->CreateIntCast(desired, i64Ty, true);
                    auto* cmpxchg = builder->CreateAtomicCmpXchg(
                        loadValue(0), expected, desired,
                        llvm::MaybeAlign(8),
                        llvm::AtomicOrdering::SequentiallyConsistent,
                        llvm::AtomicOrdering::SequentiallyConsistent);
                    return builder->CreateExtractValue(cmpxchg, 1, "atomic.cas.ok");
                }
                // Fixed-ordering atomic intrinsics (R8.1b). LLVM atomic IR
                // bakes the ordering at instruction construction; a runtime-
                // variable ordering would force a per-op switch (collapsed
                // only via the inliner, perf-unpredictable at -O0). So each
                // (op, ordering) combo is its own intrinsic + class method,
                // matching the named-method approach in cajeta.concurrent.
                // AtomicInt32 / AtomicInt64. The orderings shipped now are
                // the ones the work-stealing deque (R8.2) needs; add more
                // when a concrete consumer surfaces.
                //
                // Pattern below: small lambda factories for the four atomic
                // shapes (load / store / fetchAdd / casExpectedOK) keyed on
                // type width + ordering; each named intrinsic delegates to
                // them with its fixed ordering.
                auto atomicLoadOrd = [&](llvm::Type* ty, unsigned alignBytes,
                                          llvm::AtomicOrdering ord,
                                          const char* nameHint) -> llvm::Value* {
                    auto* load = builder->CreateLoad(ty, loadValue(0), nameHint);
                    load->setAtomic(ord);
                    load->setAlignment(llvm::Align(alignBytes));
                    return load;
                };
                auto atomicStoreOrd = [&](llvm::Type* ty, unsigned alignBytes,
                                           llvm::AtomicOrdering ord) -> llvm::Value* {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != ty) v = builder->CreateIntCast(v, ty, /*isSigned=*/true);
                    auto* store = builder->CreateStore(v, loadValue(0));
                    store->setAtomic(ord);
                    store->setAlignment(llvm::Align(alignBytes));
                    return store;
                };
                auto atomicFetchAddOrd = [&](llvm::Type* ty, unsigned alignBytes,
                                              llvm::AtomicOrdering ord) -> llvm::Value* {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != ty) v = builder->CreateIntCast(v, ty, /*isSigned=*/true);
                    return builder->CreateAtomicRMW(
                        llvm::AtomicRMWInst::Add, loadValue(0), v,
                        llvm::MaybeAlign(alignBytes), ord);
                };
                auto atomicCasOrd = [&](llvm::Type* ty, unsigned alignBytes,
                                         llvm::AtomicOrdering succ,
                                         llvm::AtomicOrdering fail) -> llvm::Value* {
                    llvm::Value* expected = loadValue(1);
                    llvm::Value* desired = loadValue(2);
                    if (expected->getType() != ty) expected = builder->CreateIntCast(expected, ty, /*isSigned=*/true);
                    if (desired->getType() != ty) desired = builder->CreateIntCast(desired, ty, /*isSigned=*/true);
                    auto* cmpxchg = builder->CreateAtomicCmpXchg(
                        loadValue(0), expected, desired,
                        llvm::MaybeAlign(alignBytes), succ, fail);
                    return builder->CreateExtractValue(cmpxchg, 1, "atomic.cas.ok");
                };
                // ---- i32 ----
                if (ns == "Cajeta" && methodCallName == "atomicI32LoadRelaxed" && parameters.size() == 1) {
                    return atomicLoadOrd(i32Ty, 4, llvm::AtomicOrdering::Monotonic, "atomic.i32.load.rlx");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32LoadAcquire" && parameters.size() == 1) {
                    return atomicLoadOrd(i32Ty, 4, llvm::AtomicOrdering::Acquire, "atomic.i32.load.acq");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32StoreRelaxed" && parameters.size() == 2) {
                    return atomicStoreOrd(i32Ty, 4, llvm::AtomicOrdering::Monotonic);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32StoreRelease" && parameters.size() == 2) {
                    return atomicStoreOrd(i32Ty, 4, llvm::AtomicOrdering::Release);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32FetchAddRelaxed" && parameters.size() == 2) {
                    return atomicFetchAddOrd(i32Ty, 4, llvm::AtomicOrdering::Monotonic);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32CompareAndSetAcquire" && parameters.size() == 3) {
                    return atomicCasOrd(i32Ty, 4,
                        llvm::AtomicOrdering::Acquire,
                        llvm::AtomicOrdering::Acquire);
                }
                // ---- i64 ----
                if (ns == "Cajeta" && methodCallName == "atomicI64LoadRelaxed" && parameters.size() == 1) {
                    return atomicLoadOrd(i64Ty, 8, llvm::AtomicOrdering::Monotonic, "atomic.i64.load.rlx");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64LoadAcquire" && parameters.size() == 1) {
                    return atomicLoadOrd(i64Ty, 8, llvm::AtomicOrdering::Acquire, "atomic.i64.load.acq");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64StoreRelaxed" && parameters.size() == 2) {
                    return atomicStoreOrd(i64Ty, 8, llvm::AtomicOrdering::Monotonic);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64StoreRelease" && parameters.size() == 2) {
                    return atomicStoreOrd(i64Ty, 8, llvm::AtomicOrdering::Release);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64FetchAddRelaxed" && parameters.size() == 2) {
                    return atomicFetchAddOrd(i64Ty, 8, llvm::AtomicOrdering::Monotonic);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64CompareAndSetAcquire" && parameters.size() == 3) {
                    return atomicCasOrd(i64Ty, 8,
                        llvm::AtomicOrdering::Acquire,
                        llvm::AtomicOrdering::Acquire);
                }
                // R9.1 — cooperative timeout. taskWaitTimeout(done_addr,
                // deadline_ns) returns 1 if *done_addr flipped before the
                // deadline, 0 if the deadline expired first. deadline_ns
                // is a CLOCK_MONOTONIC absolute timestamp; compute one via
                // currentTimeNanos() + a duration in nanos. See R9 plan
                // and Concurrency.md § withTimeout for the eventual stdlib API.
                if (ns == "Cajeta" && methodCallName == "taskWaitTimeout"
                        && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_task_wait_timeout");
                    llvm::Value* doneAddr = loadValue(0);
                    llvm::Value* deadline = loadValue(1);
                    if (deadline->getType() != i64Ty) {
                        deadline = builder->CreateIntCast(deadline, i64Ty, /*isSigned=*/true);
                    }
                    return builder->CreateCall(fn, {doneAddr, deadline});
                }
                if (ns == "Cajeta" && methodCallName == "currentTimeNanos"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_currentTimeNanos");
                    return builder->CreateCall(fn, {});
                }
                // R9.3 — surface the Task<T>'s done-flag address as a raw
                // pointer so user-level withTimeout (cajeta.concurrent.Tasks)
                // can feed it to taskWaitTimeout. The Task is heap-allocated
                // and the call arg is the pointer to it; we GEP at the
                // DONE_FIELD_INDEX defined in CajetaTask.h. The argument
                // type must resolve to a CajetaTask (Task<T>) — anything
                // else is a compile error.
                // R9.4 — I/O reactor surface. ioWait blocks the calling
                // fiber (or main thread, via direct epoll_wait) until the
                // requested event bitmask fires on fd. eventfd helpers
                // and fdClose round out the minimal Linux fd surface used
                // by R9.4's bring-up tests; they're documented as Linux-
                // only (the runtime stubs them on macOS / Windows pending
                // kqueue / IOCP).
                if (ns == "Cajeta" && methodCallName == "ioWait"
                        && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_io_wait");
                    llvm::Value* fdv = loadValue(0);
                    llvm::Value* evv = loadValue(1);
                    if (fdv->getType() != i32Ty) fdv = builder->CreateIntCast(fdv, i32Ty, true);
                    if (evv->getType() != i32Ty) evv = builder->CreateIntCast(evv, i32Ty, true);
                    return builder->CreateCall(fn, {fdv, evv});
                }
                if (ns == "Cajeta" && methodCallName == "eventfdCreate"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_eventfd_create");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "eventfdSignal"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_eventfd_signal");
                    llvm::Value* fdv = loadValue(0);
                    if (fdv->getType() != i32Ty) fdv = builder->CreateIntCast(fdv, i32Ty, true);
                    return builder->CreateCall(fn, {fdv});
                }
                if (ns == "Cajeta" && methodCallName == "eventfdConsume"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_eventfd_consume");
                    llvm::Value* fdv = loadValue(0);
                    if (fdv->getType() != i32Ty) fdv = builder->CreateIntCast(fdv, i32Ty, true);
                    return builder->CreateCall(fn, {fdv});
                }
                if (ns == "Cajeta" && methodCallName == "fdClose"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fd_close");
                    llvm::Value* fdv = loadValue(0);
                    if (fdv->getType() != i32Ty) fdv = builder->CreateIntCast(fdv, i32Ty, true);
                    return builder->CreateCall(fn, {fdv});
                }
                if (ns == "Cajeta" && methodCallName == "taskDonePointer"
                        && parameters.size() == 1) {
                    auto argExpr = dynamic_pointer_cast<Expression>(parameters[0].expression);
                    if (argExpr && !argExpr->getResolvedType()) {
                        argExpr->resolveTypes(module);
                    }
                    auto argType = argExpr ? argExpr->getResolvedType() : nullptr;
                    auto task = dynamic_pointer_cast<CajetaTask>(argType);
                    if (!task) {
                        throw Exception(
                            "Cajeta.taskDonePointer requires a Task<T> argument",
                            "CAJETA_ERROR_TYPE_MISMATCH");
                    }
                    llvm::Value* taskPtr = loadValue(0);
                    return builder->CreateStructGEP(
                        task->getLlvmType(), taskPtr,
                        CajetaTask::DONE_FIELD_INDEX, "task_done_ptr");
                }
                // R5-C / R9.3 — cooperative task cancellation. Loads the
                // task's fiber pointer and signals cancellation via
                // __cajeta_fiber_cancel with a sentinel (void*)1 throwable.
                // The body sees the cancellation at its next park/wake
                // boundary (i.e., next await) — bodies without yield
                // points run to completion regardless. The trampoline
                // catches the cancellation throw and stores it on
                // task->exception; a subsequent `await` will re-raise,
                // letting callers wrap it in try/catch. Used by
                // cajeta.concurrent.Tasks.withTimeout to terminate slow
                // bodies on deadline rather than letting them run to
                // natural completion.
                if (ns == "Cajeta" && methodCallName == "taskCancel"
                        && parameters.size() == 1) {
                    auto argExpr = dynamic_pointer_cast<Expression>(parameters[0].expression);
                    if (argExpr && !argExpr->getResolvedType()) {
                        argExpr->resolveTypes(module);
                    }
                    auto argType = argExpr ? argExpr->getResolvedType() : nullptr;
                    auto task = dynamic_pointer_cast<CajetaTask>(argType);
                    if (!task) {
                        throw Exception(
                            "Cajeta.taskCancel requires a Task<T> argument",
                            "CAJETA_ERROR_TYPE_MISMATCH");
                    }
                    llvm::Value* taskPtr = loadValue(0);
                    llvm::Type* ptrTy2 = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Value* fiberSlot = builder->CreateStructGEP(
                        task->getLlvmType(), taskPtr,
                        CajetaTask::FIBER_FIELD_INDEX, "task_fiber_slot");
                    llvm::Value* fiberPtr = builder->CreateLoad(
                        ptrTy2, fiberSlot, "task_fiber");
                    llvm::Function* cancelFn = module->getRuntimeFunction(
                        "__cajeta_fiber_cancel");
                    // Sentinel (void*)1 — same shape as `throw 1` produces,
                    // catches with `catch (Exception e)` reading `(int32) e`.
                    llvm::Value* sentinel = builder->CreateIntToPtr(
                        llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(llvmCtx), 1), ptrTy2);
                    return builder->CreateCall(cancelFn, {fiberPtr, sentinel});
                }
                // R9.5 — cooperative fiber sleep. Parks the running fiber
                // on the timer wheel for `nanos` nanoseconds (built atop
                // __cajeta_task_wait_timeout against a sentinel done flag
                // that never flips, so the deadline is the only wake).
                // Used by Channel.select's poll-and-backoff loop and
                // available to any caller wanting a cooperative sleep
                // without burning CPU.
                if (ns == "Cajeta" && methodCallName == "fiberSleepNanos"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_sleep_nanos");
                    llvm::Value* nanos = loadValue(0);
                    if (nanos->getType() != i64Ty) {
                        nanos = builder->CreateIntCast(nanos, i64Ty, /*isSigned=*/true);
                    }
                    return builder->CreateCall(fn, {nanos});
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
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(lm, id, {opTy});
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
                // Post Phase 2b-β, the result wraps into a class
                // `cajeta.lang.String` instance so the caller can dispatch
                // class methods (`.equals`, `.count`, …) against it. The
                // raw `char*` from the legacy runtime helpers ends up as
                // the byte content; `wrapCStringIntoClassString` copies
                // those bytes into the class String's CajetaArray and
                // frees the intermediate (or skips the free for static
                // literals like `__cajeta_bool_to_str`'s "true"/"false").
                if ((ns == "Integer" || ns == "Long" || ns == "Double" || ns == "Boolean")
                        && methodCallName == "toString" && parameters.size() == 1) {
                    llvm::Value* v = loadValue(0);
                    llvm::Type* t = v->getType();
                    // Pin resolvedType on the way out so a parent call site
                    // (e.g. `Integer.parseInt(Integer.toString(x))`) sees
                    // the class String type and routes through
                    // `loadStringArg`'s class-String unwrap.
                    resolvedType = CajetaType::of("String");
                    if (ns == "Boolean" || t->isIntegerTy(1)) {
                        if (t->isIntegerTy(1)) v = builder->CreateZExt(v, i32Ty);
                        else if (t != i32Ty && t->isIntegerTy()) v = builder->CreateIntCast(v, i32Ty, true);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr,
                            "boolStr", /*freeAfterWrap=*/false);
                    }
                    if (t->isFloatingPointTy() || ns == "Double") {
                        if (t != f64Ty) v = t->isIntegerTy()
                            ? builder->CreateSIToFP(v, f64Ty)
                            : builder->CreateFPCast(v, f64Ty);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "f64Str");
                    }
                    // Integer/Long path.
                    if (t->isIntegerTy() && t != i64Ty) v = builder->CreateIntCast(v, i64Ty, true);
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                    llvm::Value* cstr = builder->CreateCall(fn, {v});
                    return wrapCStringIntoClassString(module, cstr, "i64Str");
                }
                if (ns == "String" && methodCallName == "valueOf" && parameters.size() == 1) {
                    llvm::Value* v = loadValue(0);
                    llvm::Type* t = v->getType();
                    resolvedType = CajetaType::of("String");
                    if (t->isPointerTy()) return v;  // already a String / ptr
                    if (t->isIntegerTy(1)) {
                        v = builder->CreateZExt(v, i32Ty);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr,
                            "valueOfBool", /*freeAfterWrap=*/false);
                    }
                    // Cajeta `char` is an i32 codepoint (since the
                    // 2026-05-18 redefinition). For ASCII codepoints
                    // (0..127) the UTF-8 encoding is the same single
                    // byte, so we can lower through the i8 path
                    // `__cajeta_str_fromChar`. Wider codepoints would
                    // need a multibyte encoder; not in v1.
                    auto argTy = parameters[0].expression
                        ? std::dynamic_pointer_cast<Expression>(parameters[0].expression)
                          : nullptr;
                    auto argResolved = argTy ? argTy->getResolvedType() : nullptr;
                    bool isCharArg = argResolved && argResolved->getQName()
                        && argResolved->getQName()->getTypeName() == "char";
                    if (isCharArg && t->isIntegerTy() && t != llvm::Type::getInt8Ty(llvmCtx)) {
                        // Narrow codepoint to i8 (ASCII-only v1).
                        v = builder->CreateIntCast(v,
                            llvm::Type::getInt8Ty(llvmCtx), /*isSigned=*/true);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_fromChar");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "valueOfChar");
                    }
                    if (t->isIntegerTy(8)) {
                        // Treat i8 as char for String.valueOf — single-byte string.
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_fromChar");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "valueOfChar");
                    }
                    if (t->isIntegerTy()) {
                        if (t != i64Ty) v = builder->CreateIntCast(v, i64Ty, true);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "valueOfInt");
                    }
                    if (t->isFloatingPointTy()) {
                        if (t != f64Ty) v = builder->CreateFPCast(v, f64Ty);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "valueOfF64");
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
            // REFL-1.6: `obj.getClass()` — the object's dynamic Class. Synthesized
            // as a call to __cajeta_object_get_class(obj) (the same native that
            // backs Class.of), returning Class<?>. No method is declared on
            // Object — doing this here sidesteps the Object→cajeta.reflect
            // bootstrap cycle. Gated on a class-instance receiver, no args, and
            // the class not declaring its own getClass (a user override wins).
            // Must run BEFORE the generic invokeMethod dispatch below, which
            // would otherwise find no `getClass` method and resolve to null.
            // The wildcard return Class<?> is the force-built canonical
            // instantiation (REFL-1.7).
            if (methodCallName == "getClass" && parameters.empty()
                    && receiverType) {
                auto recvCls = dynamic_pointer_cast<CajetaClass>(receiverType);
                if (recvCls && !recvCls->isInterface()
                        && recvCls->getMethods().find("getClass")
                                == recvCls->getMethods().end()) {
                    auto& cmap = CajetaType::getCanonicalMap();
                    auto wcIt = cmap.find("cajeta.reflect.Class<?>");
                    if (wcIt != cmap.end() && wcIt->second) {
                        llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                        llvm::Value* objPtr =
                            loadIfLValue(module, receiver, exprChild);
                        llvm::FunctionType* ft =
                            llvm::FunctionType::get(ptrTy, {ptrTy}, false);
                        llvm::FunctionCallee fn =
                            module->getLlvmModule()->getOrInsertFunction(
                                "__cajeta_object_get_class", ft);
                        llvm::Value* co = builder->CreateCall(fn, {objPtr},
                            "refl.getClass");
                        resolvedType = wcIt->second;
                        return co;
                    }
                }
            }
            // Vector geometry methods: a.dot(b) -> T, v.length() -> T,
            // v.normalize() -> Vector. Intercepted on a CajetaVector receiver
            // before the generic class-method dispatch (vectors aren't classes).
            if (auto vecT = dynamic_pointer_cast<CajetaVector>(receiverType)) {
                llvm::Value* self = loadIfLValue(module, receiver, exprChild);
                bool isFloat = vecT->getElementType()->getLlvmType()
                                   ->isFloatingPointTy();
                // A boolean-element vector is a comparison MASK (`<N x i1>`):
                // reduce with all()/any(), blend with select(a, b). Masks have
                // no arithmetic/geometry methods.
                bool isMask = vecT->getElementType()->getLlvmType()
                                  ->isIntegerTy(1);
                if (isMask) {
                    if (methodCallName == "all" || methodCallName == "any") {
                        if (!parameters.empty()) {
                            throw Exception(
                                "Vector mask " + methodCallName
                                + " takes no arguments",
                                "CAJETA_ERROR_VECTOR_METHOD");
                        }
                        resolvedType = CajetaType::of("boolean");
                        return methodCallName == "all"
                            ? builder->CreateAndReduce(self)
                            : builder->CreateOrReduce(self);
                    }
                    if (methodCallName == "select") {
                        if (parameters.size() != 2) {
                            throw Exception(
                                "Vector mask select expects 2 arguments "
                                "(whenTrue, whenFalse)",
                                "CAJETA_ERROR_VECTOR_METHOD");
                        }
                        llvm::Value* a = loadIfLValue(module,
                            parameters[0].expression->generateCode(module),
                            parameters[0].expression);
                        llvm::Value* b = loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression);
                        if (!parameters[0].expression->getResolvedType())
                            parameters[0].expression->resolveTypes(module);
                        resolvedType = parameters[0].expression->getResolvedType();
                        return builder->CreateSelect(self, a, b, "vec.select");
                    }
                    throw Exception(
                        "Vector mask has no method '" + methodCallName + "'",
                        "CAJETA_ERROR_VECTOR_METHOD");
                }
                if (methodCallName == "dot") {
                    if (isFloat) {
                        if (parameters.size() != 1) {
                            throw Exception("Vector.dot expects 1 argument",
                                            "CAJETA_ERROR_VECTOR_METHOD");
                        }
                        llvm::Value* other = loadIfLValue(module,
                            parameters[0].expression->generateCode(module),
                            parameters[0].expression);
                        resolvedType = vecT->getElementType();
                        return vecops::dot(*builder, self, other, true);
                    }
                    // Integer dot (DP4a): Vector<int8,4>/<uint8,4> -> int32 with
                    // an optional int32 accumulator. The host accumulates via the
                    // portable widening reduce; the device Vulkan path uses the
                    // hardware DP4a op (results are identical).
                    auto* ivt = llvm::cast<llvm::FixedVectorType>(self->getType());
                    if (ivt->getNumElements() != 4 ||
                        ivt->getElementType()->getIntegerBitWidth() != 8) {
                        throw Exception(
                            "integer Vector.dot currently supports 4-lane 8-bit "
                            "vectors (DP4a): Vector<int8,4> / Vector<uint8,4>",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (parameters.size() != 1 && parameters.size() != 2) {
                        throw Exception(
                            "Vector.dot expects (other) or (other, acc)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Type* i32 =
                        llvm::Type::getInt32Ty(builder->getContext());
                    llvm::Value* acc;
                    if (parameters.size() == 2) {
                        llvm::Value* a2 = loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression);
                        acc = builder->CreateIntCast(a2, i32, /*isSigned=*/true,
                                                     "dot.acc");
                    } else {
                        acc = llvm::ConstantInt::get(i32, 0);
                    }
                    bool sgn = (vecT->getElementType()->getTypeFlags()
                                & SIGNED_FLAG) != 0;
                    resolvedType = CajetaType::of("int32");
                    return vecops::idotWiden(*builder, self, other, acc, sgn);
                }
                // SIMD: eqMask(needle) -> int32. Per-lane equality packed into a
                // bitmask (bit i set iff lane i == needle) — the JSON-scanner
                // workhorse. Pairs with Cajeta.ctz64 to find the first match.
                if (methodCallName == "eqMask") {
                    if (parameters.size() != 1) {
                        throw Exception("Vector.eqMask expects 1 argument",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* needle = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* nEl = vecops::coerceScalar(*builder, needle,
                        vecT->getElementType()->getLlvmType());
                    llvm::Value* bits = vecops::eqMask(*builder, self, nEl);
                    resolvedType = CajetaType::of("int32");
                    return builder->CreateZExt(bits,
                        llvm::Type::getInt32Ty(builder->getContext()),
                        "eqmask.i32");
                }
                if (methodCallName == "length") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector.length requires a floating-point element type",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = vecT->getElementType();
                    return vecops::length(*builder, self);
                }
                if (methodCallName == "normalize") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector.normalize requires a floating-point element type",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = vecT;
                    return vecops::normalize(*builder, self);
                }
                // B1 intrinsics A1 — element-wise min/max/clamp/lerp. v1 is
                // float-only (symmetric with the device path, which does not yet
                // track per-vector element signedness for the integer smin/umin
                // split); the vecops helpers are signedness-general for the
                // integer follow-on.
                llvm::Type* elemLlvm = vecT->getElementType()->getLlvmType();
                bool isSigned =
                    (vecT->getElementType()->getTypeFlags() & SIGNED_FLAG) != 0;
                if (methodCallName == "min" || methodCallName == "max") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector." + methodCallName + " requires a "
                            "floating-point element type (integer min/max is a "
                            "follow-on)", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (parameters.size() != 1) {
                        throw Exception(
                            "Vector." + methodCallName + " expects 1 argument",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    resolvedType = vecT;
                    return methodCallName == "min"
                        ? vecops::vmin(*builder, self, other, isFloat, isSigned)
                        : vecops::vmax(*builder, self, other, isFloat, isSigned);
                }
                if (methodCallName == "clamp") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector.clamp requires a floating-point element type "
                            "(integer clamp is a follow-on)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Vector.clamp expects 2 arguments (lo, hi)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* lo = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[0].expression->generateCode(module),
                            parameters[0].expression), elemLlvm);
                    llvm::Value* hi = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression), elemLlvm);
                    resolvedType = vecT;
                    return vecops::clamp(*builder, self, lo, hi, isFloat, isSigned);
                }
                if (methodCallName == "lerp") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector.lerp requires a floating-point element type",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Vector.lerp expects 2 arguments (other, t)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* t = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression), elemLlvm);
                    resolvedType = vecT;
                    return vecops::lerp(*builder, self, other, t);
                }
                // B1 intrinsics A2 — geometry: cross (3-D)/reflect/refract
                // (Vector results) and distance (scalar). All float-only.
                if (methodCallName == "cross" || methodCallName == "reflect"
                        || methodCallName == "refract"
                        || methodCallName == "distance") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector." + methodCallName + " requires a "
                            "floating-point element type",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (methodCallName == "cross" && vecT->getLanes() != 3) {
                        throw Exception(
                            "Vector.cross requires 3-component vectors (got "
                            + vecT->toCanonical() + ")",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    size_t want = methodCallName == "refract" ? 2 : 1;
                    if (parameters.size() != want) {
                        throw Exception(
                            "Vector." + methodCallName + " expects "
                            + std::to_string(want) + " argument(s)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    if (methodCallName == "cross") {
                        resolvedType = vecT;
                        return vecops::cross(*builder, self, other);
                    }
                    if (methodCallName == "reflect") {
                        resolvedType = vecT;
                        return vecops::reflect(*builder, self, other);
                    }
                    if (methodCallName == "distance") {
                        resolvedType = vecT->getElementType();
                        return vecops::distance(*builder, self, other);
                    }
                    // refract(n, eta)
                    llvm::Value* eta = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression), elemLlvm);
                    resolvedType = vecT;
                    return vecops::refract(*builder, self, other, eta);
                }
                throw Exception(
                    "Vector has no method '" + methodCallName + "'",
                    "CAJETA_ERROR_VECTOR_METHOD");
            }
            // Matrix methods (B1): m.transpose() -> Matrix<T,C,R>, m.identity()
            // (R==C) -> Matrix<T,R,C>, m.row(r) -> Vector<T,C>, m.col(c) ->
            // Vector<T,R>, m.hadamard(b) -> Matrix<T,R,C>. Intercepted on a
            // CajetaMatrix receiver before the generic class dispatch (the
            // declared Matrix class's bodies are placeholders).
            if (auto matT = dynamic_pointer_cast<CajetaMatrix>(receiverType)) {
                llvm::Value* self = loadIfLValue(module, receiver, exprChild);
                bool isFloat = matT->getElementType()->getLlvmType()
                                   ->isFloatingPointTy();
                unsigned R = matT->getRows(), C = matT->getCols();
                // A boolean-element matrix is a comparison MASK (`<R*C x i1>`):
                // reduce with all()/any() (whole-matrix `(a==b).all()`), blend
                // with select(a, b).
                if (matT->getElementType()->getLlvmType()->isIntegerTy(1)) {
                    if (methodCallName == "all" || methodCallName == "any") {
                        if (!parameters.empty()) {
                            throw Exception(
                                "Matrix mask " + methodCallName
                                + " takes no arguments",
                                "CAJETA_ERROR_MATRIX_METHOD");
                        }
                        resolvedType = CajetaType::of("boolean");
                        return methodCallName == "all"
                            ? builder->CreateAndReduce(self)
                            : builder->CreateOrReduce(self);
                    }
                    if (methodCallName == "select") {
                        if (parameters.size() != 2) {
                            throw Exception(
                                "Matrix mask select expects 2 arguments",
                                "CAJETA_ERROR_MATRIX_METHOD");
                        }
                        llvm::Value* a = loadIfLValue(module,
                            parameters[0].expression->generateCode(module),
                            parameters[0].expression);
                        llvm::Value* b = loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression);
                        if (!parameters[0].expression->getResolvedType())
                            parameters[0].expression->resolveTypes(module);
                        resolvedType = parameters[0].expression->getResolvedType();
                        return builder->CreateSelect(self, a, b, "mat.select");
                    }
                    throw Exception(
                        "Matrix mask has no method '" + methodCallName + "'",
                        "CAJETA_ERROR_MATRIX_METHOD");
                }
                if (methodCallName == "transpose") {
                    resolvedType = CajetaMatrix::getOrCreate(
                        module, matT->getElementType(), C, R);
                    return matops::transpose(*builder, self, R, C);
                }
                if (methodCallName == "identity") {
                    if (R != C) {
                        throw Exception(
                            "Matrix.identity requires a square matrix (got "
                            + matT->toCanonical() + ")",
                            "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    resolvedType = matT;
                    return matops::identity(
                        *builder, matT->getElementType()->getLlvmType(), R);
                }
                if (methodCallName == "row" || methodCallName == "col") {
                    if (parameters.size() != 1) {
                        throw Exception(
                            "Matrix." + methodCallName + " expects 1 argument",
                            "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    llvm::Value* idx = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Type* i32Ty =
                        llvm::Type::getInt32Ty(*module->getLlvmContext());
                    if (idx->getType() != i32Ty)
                        idx = builder->CreateIntCast(idx, i32Ty, false,
                                                     "mat.meth.idx");
                    if (methodCallName == "row") {
                        resolvedType = CajetaVector::getOrCreate(
                            module, matT->getElementType(), C);
                        return matops::row(*builder, self, R, C, idx);
                    }
                    resolvedType = CajetaVector::getOrCreate(
                        module, matT->getElementType(), R);
                    return matops::col(*builder, self, R, C, idx);
                }
                if (methodCallName == "hadamard") {
                    if (parameters.size() != 1) {
                        throw Exception("Matrix.hadamard expects 1 argument",
                                        "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    auto other = parameters[0].expression;
                    llvm::Value* b = loadIfLValue(module,
                        other->generateCode(module), other);
                    resolvedType = matT;
                    return matops::hadamard(*builder, self, b, isFloat);
                }
                // determinant() -> scalar, inverse() -> Matrix<T,N,N>. Square
                // n in {2,3,4}, float element only.
                if (methodCallName == "determinant"
                        || methodCallName == "inverse") {
                    if (R != C || R < 2 || R > 4) {
                        throw Exception(
                            "Matrix." + methodCallName + " requires a square "
                            "2x2, 3x3, or 4x4 matrix (got " + matT->toCanonical()
                            + ")", "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    if (!isFloat) {
                        throw Exception(
                            "Matrix." + methodCallName + " requires a "
                            "floating-point element type",
                            "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    if (!parameters.empty()) {
                        throw Exception(
                            "Matrix." + methodCallName + " takes no arguments",
                            "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    if (methodCallName == "determinant") {
                        resolvedType = matT->getElementType();
                        return matops::determinant(*builder, self, R);
                    }
                    resolvedType = matT;
                    return matops::inverse(*builder, self, R);
                }
                throw Exception(
                    "Matrix has no method '" + methodCallName + "'",
                    "CAJETA_ERROR_MATRIX_METHOD");
            }
            // Quaternion methods: normalize/conjugate/length/dot/nlerp. (slerp is
            // a device-transcendental follow-on.) Lowered via quatops/vecops.
            if (auto qT = dynamic_pointer_cast<CajetaQuaternion>(receiverType)) {
                llvm::Value* self = loadIfLValue(module, receiver, exprChild);
                if (methodCallName == "normalize") {
                    resolvedType = qT;
                    return vecops::normalize(*builder, self);
                }
                if (methodCallName == "conjugate") {
                    resolvedType = qT;
                    return quatops::conjugate(*builder, self);
                }
                if (methodCallName == "length") {
                    resolvedType = qT->getElementType();
                    return vecops::length(*builder, self);
                }
                if (methodCallName == "dot") {
                    if (parameters.size() != 1) {
                        throw Exception("Quaternion.dot expects 1 argument",
                                        "CAJETA_ERROR_QUATERNION_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    resolvedType = qT->getElementType();
                    return vecops::dot(*builder, self, other, /*isFloat=*/true);
                }
                if (methodCallName == "nlerp") {
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Quaternion.nlerp expects 2 arguments (other, t)",
                            "CAJETA_ERROR_QUATERNION_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* t = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression),
                        qT->getElementType()->getLlvmType());
                    resolvedType = qT;
                    return quatops::nlerp(*builder, self, other, t);
                }
                if (methodCallName == "slerp") {
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Quaternion.slerp expects 2 arguments (other, t)",
                            "CAJETA_ERROR_QUATERNION_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* t = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression),
                        qT->getElementType()->getLlvmType());
                    // Host: sin/acos via the llvm intrinsics (libm under the JIT).
                    quatops::TrigEmitter trig =
                        [&](const std::string& nm,
                            llvm::ArrayRef<llvm::Value*> as) -> llvm::Value* {
                            return builder->CreateUnaryIntrinsic(
                                nm == "sin" ? llvm::Intrinsic::sin
                                            : llvm::Intrinsic::acos, as[0]);
                        };
                    resolvedType = qT;
                    return quatops::slerp(*builder, self, other, t, trig);
                }
                throw Exception(
                    "Quaternion has no method '" + methodCallName + "'",
                    "CAJETA_ERROR_QUATERNION_METHOD");
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
                    // Three alloca shapes can show up here:
                    //   * ptr / primitive slot — a StackField for a class
                    //     local stores the heap (or sret-slot) pointer in a
                    //     `ptr`-typed slot; load-through materializes the
                    //     instance pointer for dispatch.
                    //   * struct slot — an sret slot from a chained
                    //     value-returning MCE (e.g. `Duration.ofMillis(3)
                    //     .toNanos()`). The alloca IS the in-place value's
                    //     address; load-through would yield the struct value
                    //     and the downstream vtable load / `this` pass would
                    //     receive a non-pointer (task #46, M5(b) chained-sret
                    //     gap). Skip the load — the slot pointer is the address.
                    //   * @ValueType receiver — its slot holds the aggregate
                    //     INLINE (alloca %VtPoint — struct OR vector, not
                    //     alloca ptr). Its instance methods take `this` BY
                    //     POINTER (the slot address), so loading would pass the
                    //     aggregate by value and mismatch the `this:pointer`
                    //     signature (the JIT-verify failure). Pass the slot
                    //     address. Mirrors the value-type guards in operator[]
                    //     dispatch (Expression.cpp) and DotExpression (S2).
                    auto recvCls = dynamic_pointer_cast<CajetaClass>(receiverType);
                    bool valueTypeReceiver = recvCls && recvCls->isValueType()
                        && !a->getAllocatedType()->isPointerTy();
                    bool sretStructSlot = a->getAllocatedType()->isStructTy();
                    if (!valueTypeReceiver && !sretStructSlot) {
                        receiver = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                } else if (dynamic_pointer_cast<ArrayIndexExpression>(exprChild)) {
                    // Class-ref array elements store an 8-byte `ptr` to the heap
                    // instance — load through to the dispatch receiver. Interface
                    // elements are 24-byte fat-pointer bodies stored INLINE, so
                    // the element GEP already points AT the body; loading would
                    // yield the data word and the interface dispatch path would
                    // deref it as a body → garbage vtable → crash. Skip the load
                    // for interfaces (mirrors the field-receiver branch below).
                    auto elemRc = dynamic_pointer_cast<CajetaClass>(receiverType);
                    bool elemIsInterface = elemRc && elemRc->isInterface();
                    if (!elemIsInterface) {
                        receiver = builder->CreateLoad(
                            llvm::PointerType::get(*module->getLlvmContext(), 0), receiver);
                    }
                } else if ((dynamic_pointer_cast<DotExpression>(exprChild)
                            || dynamic_pointer_cast<IdentifierExpression>(exprChild))
                        && (llvm::isa<llvm::GetElementPtrInst>(receiver)
                            || llvm::isa<llvm::GlobalVariable>(receiver))
                        && receiverType
                        && dynamic_pointer_cast<CajetaClass>(receiverType)
                        && !dynamic_pointer_cast<CajetaView>(receiverType)) {
                    // Chained field access `a.b.method()` where `b` is a
                    // class-ref OR array field, OR a STATIC field receiver —
                    // qualified `Class.STATIC.method()` (DotExpression) or bare
                    // `STATIC.method()` (IdentifierExpression, same-class static
                    // shorthand / implicit-this instance field). The receiver is
                    // the field's slot pointer — a GEP for an instance field, or
                    // the static-field GlobalVariable (its address) for a
                    // static field. Either way the slot stores a `ptr` to the
                    // referent instance / array header (per
                    // CajetaClass::fieldLayoutType rule that lays class-ref
                    // and array fields as `ptr`). Load through to
                    // materialize the instance/header pointer used as the
                    // dispatch receiver. Without this the vtable load at
                    // instance[0] would read the SLOT's first word (which
                    // is the instance ptr itself, not the vtable), and
                    // __cajeta_vtable_lookup would walk garbage; for arrays
                    // the same shape — `.count()` / `.stream()` would read
                    // the slot's first 8 bytes (the header pointer) as if
                    // it were the count word. (The static-field case
                    // previously fell through with no load → the global's
                    // address was passed as `this` → SIGSEGV.) View and
                    // interface fields stay inline, so the slot IS the
                    // language-level value — skip the load there.
                    auto rc = dynamic_pointer_cast<CajetaClass>(receiverType);
                    if (!rc->isInterface()) {
                        receiver = builder->CreateLoad(
                            llvm::PointerType::get(*module->getLlvmContext(), 0),
                            receiver);
                    }
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
                    if (it != cmap.end()) {
                        if (auto cls = dynamic_pointer_cast<CajetaClass>(it->second)) {
                            // REFL-1.7: a static call on a bare TEMPLATE name
                            // (e.g. `Class.of(...)`, `Class.forName(...)`). The
                            // template itself is never built, so static dispatch
                            // against it would silently resolve to null. Static
                            // methods don't depend on the type arguments, so
                            // route through the canonical all-wildcard
                            // instantiation (Class<?>), which is fully built.
                            if (cls->isTemplate()) {
                                std::vector<CajetaTypePtr> wildArgs;
                                for (size_t i = 0;
                                        i < cls->getTypeParameters().size(); ++i) {
                                    wildArgs.push_back(
                                        CajetaType::wildcardSentinel());
                                }
                                auto inst = cls->instantiate(wildArgs);
                                receiverType = inst ? std::static_pointer_cast<
                                    CajetaType>(inst) : it->second;
                            } else {
                                receiverType = it->second;
                            }
                        }
                    }
                }
            }
        }

        // ----- cajeta.math.DType.codeOf<T>() intrinsic -----
        // Fold T's reified dtype to a constant i32 packing its descriptor:
        //   code = (kind << 16) | (bits << 4) | variant
        // (kind: BOOL=0, INT=1, UINT=2, FLOAT=3; variant distinguishes
        // same-width floats — bf16 / the fp8 / fp6 / fp4 encodings). The
        // cajeta-side DType.of<T>() decodes this into a DType — the type→DType
        // bridge Tensor<T>.dtype() rides. Mirrors the Matrix/Vector intercepts:
        // detect by receiver FQN + method name, emit custom IR, early-return.
        if (receiverType && methodCallName == "codeOf"
                && receiverType->toCanonical() == "cajeta.math.DType"
                && explicitMethodTypeArgs.size() == 1
                && explicitMethodTypeArgs[0]) {
            CajetaTypePtr dtypeT = explicitMethodTypeArgs[0];
            CajetaTypeFlags f = dtypeT->getTypeFlags();
            uint64_t typeId = (uint64_t) (f & TYPE_ID_MASK);
            int32_t kind = 0;
            int32_t bits = 0;
            int32_t variant = 0;
            if (typeId == (uint64_t) BOOLEAN_ID) {
                kind = 0;           // KIND_BOOL
                bits = 8;
            } else {
                if (f & BIT_4_FLAG) bits = 4;
                else if (f & BIT_6_FLAG) bits = 6;
                else if (f & BIT_8_FLAG) bits = 8;
                else if (f & BIT_16_FLAG) bits = 16;
                else if (f & BIT_32_FLAG) bits = 32;
                else if (f & BIT_64_FLAG) bits = 64;
                else if (f & BIT_128_FLAG) bits = 128;
                if (f & FLOAT_FLAG) {
                    kind = 3;       // KIND_FLOAT
                    if (typeId == (uint64_t) BFLOAT16_ID) variant = 1;
                    else if (typeId == (uint64_t) FLOAT8E4M3_ID) variant = 2;
                    else if (typeId == (uint64_t) FLOAT8E5M2_ID) variant = 3;
                    else if (typeId == (uint64_t) FLOAT8E4M3FNUZ_ID) variant = 4;
                    else if (typeId == (uint64_t) FLOAT8E5M2FNUZ_ID) variant = 5;
                    else if (typeId == (uint64_t) FLOAT6E2M3_ID) variant = 6;
                    else if (typeId == (uint64_t) FLOAT6E3M2_ID) variant = 7;
                    else if (typeId == (uint64_t) FLOAT4E2M1_ID) variant = 8;
                    // else standard f16/f32/f64/f128 → variant 0
                } else if (f & INT_FLAG) {
                    kind = (f & SIGNED_FLAG) ? 1 : 2;   // KIND_INT / KIND_UINT
                }
                // else: not a numeric primitive → best-effort (kind 0).
            }
            int32_t code = (kind << 16) | (bits << 4) | variant;
            resolvedType = CajetaType::of("int32");
            return llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(llvmCtx), (uint64_t) (uint32_t) code);
        }

        // Function-typed field invocation: `recv.fieldName(args)` where
        // fieldName is a CajetaFunctionType-typed property on recv's
        // class. Same closure layout as the bare-name local-lambda
        // path above ({ ptr fn, ptr captures, ptr drop_fn }); the
        // only difference is sourcing the closure-ptr from a GEP'd
        // field slot rather than a stack alloca. Without this branch
        // the lookup falls through to invokeMethod, which has no
        // method named fieldName, returns null, and downstream
        // codegen silently emits a default (e.g. `i1 false` for a
        // boolean-typed call result) — masking the missing call as a
        // wrong-answer bug rather than a hard error.
        if (receiver && receiverType) {
            auto recvClass = dynamic_pointer_cast<CajetaClass>(receiverType);
            // A real METHOD named `methodCallName` takes precedence over a
            // same-named function-typed FIELD. The builder idiom declares both
            // — a `handler` closure field AND a `handler(fn)` setter — and
            // `b.handler(fn)` must call the SETTER, not try to invoke the
            // (often null) field-closure. Without this guard the field path
            // below greedily wins: it evaluates the arg eagerly (a bare-param
            // lambda then fails inference before the method's expectedType
            // propagator runs) and, at runtime, calls through the null field
            // slot. Only fall into field-invocation when no method shadows it.
            bool sameNamedMethod = false;
            if (recvClass) {
                std::function<bool(const CajetaClassPtr&)> hasMethod =
                    [&](const CajetaClassPtr& cls) -> bool {
                        for (auto& mEntry : cls->getMethods()) {
                            if (mEntry.second
                                    && mEntry.second->getName() == methodCallName
                                    && !mEntry.second->isConstructor()) {
                                return true;
                            }
                        }
                        for (auto& parent : cls->getSuperClasses()) {
                            if (hasMethod(parent)) return true;
                        }
                        return false;
                    };
                sameNamedMethod = hasMethod(recvClass);
            }
            if (recvClass && !sameNamedMethod) {
                StructurePropertyPtr fnField;
                CajetaClassPtr fieldOwner;
                std::function<bool(const CajetaClassPtr&)> findFnField =
                    [&](const CajetaClassPtr& cls) -> bool {
                        auto& props = cls->getProperties();
                        auto it = props.find(methodCallName);
                        if (it != props.end()
                                && dynamic_pointer_cast<CajetaFunctionType>(
                                    it->second->getType())) {
                            fnField = it->second;
                            fieldOwner = cls;
                            return true;
                        }
                        for (auto& parent : cls->getSuperClasses()) {
                            if (findFnField(parent)) return true;
                        }
                        return false;
                    };
                if (findFnField(recvClass)) {
                    auto fnType = dynamic_pointer_cast<CajetaFunctionType>(
                        fnField->getType());
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::StructType* closureTy = llvm::StructType::get(
                        llvmCtx, {ptrTy, ptrTy, ptrTy});
                    unsigned fieldIdx =
                        (unsigned) fieldOwner->getFieldLlvmIndex(fnField);
                    llvm::Value* slot = builder->CreateStructGEP(
                        recvClass->getLlvmType(), receiver, fieldIdx,
                        methodCallName + "_slot");
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
                    // M5(b) — sret form: allocate the caller-owned result
                    // slot and thread it as the closure's hidden arg 0;
                    // call returns void, MCE value is the slot pointer.
                    llvm::Value* sretSlot = nullptr;
                    auto retClass = dynamic_pointer_cast<CajetaClass>(fnType->getReturnType());
                    if (fnType->usesSret() && retClass) {
                        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
                        llvm::IRBuilder<> entryBuilder(
                            &curFn->getEntryBlock(),
                            curFn->getEntryBlock().begin());
                        sretSlot = entryBuilder.CreateAlloca(
                            retClass->getLlvmType(), nullptr, "fn_sret");
                    }
                    vector<llvm::Value*> args;
                    if (sretSlot) args.push_back(sretSlot);
                    args.push_back(captures);
                    size_t baseIdx = sretSlot ? 2 : 1;
                    llvm::FunctionType* sig = fnType->getLlvmFunctionType();
                    for (size_t i = 0; i < parameters.size(); ++i) {
                        llvm::Value* v = parameters[i].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                            v = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        size_t sigIdx = baseIdx + i;
                        if (sig && sigIdx < sig->getNumParams() && v
                                && v->getType() != sig->getParamType(sigIdx)) {
                            llvm::Type* expected = sig->getParamType(sigIdx);
                            if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
                                v = builder->CreateIntCast(v, expected, /*isSigned=*/true);
                            } else if (expected->isFloatingPointTy()
                                    && v->getType()->isFloatingPointTy()) {
                                v = builder->CreateFPCast(v, expected);
                            } else if (expected->isPointerTy()
                                    && !v->getType()->isPointerTy()) {
                                v = spillAggregateForByPointerArg(module, v);
                            }
                        }
                        args.push_back(v);
                    }
                    // Pin resolvedType so a caller using this MCE as an
                    // argument can recover the lambda's declared return
                    // type (mirrors the post-invokeMethod resolvedType
                    // pinning further below).
                    if (fnType->getReturnType()) {
                        resolvedType = fnType->getReturnType();
                    }
                    llvm::CallInst* call = builder->CreateCall(sig, callee, args);
                    if (sretSlot && retClass) {
                        call->addParamAttr(0, llvm::Attribute::get(
                            llvmCtx, llvm::Attribute::StructRet,
                            retClass->getLlvmType()));
                        return sretSlot;
                    }
                    return call;
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

        // String built-in methods. After Phase 2b-β the canonical
        // `cajeta.lang.String` is a class and string literals
        // materialize as class instances — so this intrinsic table
        // applies ONLY when the receiver is a raw pointer that ISN'T a
        // CajetaClass (the legacy i8* bootstrap path: runtime
        // bring-up before class String is loaded, or any call site
        // whose receiver isn't typed as String at all but is a bare
        // pointer-typed expression). Class-typed receivers route
        // through normal method dispatch and hit String's Cajeta-level
        // methods (isEmpty, equals, charAt, indexOf, startsWith,
        // endsWith, contains, substring, toLowerCase, toUpperCase,
        // trim, replace, size — all defined on the class).
        bool receiverIsString = false;
        if (receiver && receiver->getType()->isPointerTy()
                && !dynamic_pointer_cast<CajetaClass>(receiverType)) {
            // Bare-pointer receiver, not a known class. Excludes
            // CajetaArray (which inherits CajetaClass — array's size()
            // routes through the dedicated structural-accessor branch
            // below) and class String (whose methods now live on the
            // class). This covers the chained-pointer case where the
            // inner call hasn't populated resolvedType yet AND legacy
            // i8* C-string flows from runtime symbols.
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
            if (methodCallName == "size") {
                // size() returns byte length of storage; count()
                // returns codepoint count and routes through normal
                // method resolution (cajeta.lang.String.count() walks
                // the UTF-8). length() is intentionally not accepted
                // — collection-style accessors use count() everywhere.
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

        // Structural accessor on primitive arrays. `count()` is the
        // sole name — matches the convention every collection in the
        // language uses. Reads the array header's first field
        // (i64 count). Class-typed receivers fall through to normal
        // method resolution so `ArrayList<T>.count()` etc. dispatch
        // through the regular vtable / static-method path.
        if (receiver && methodCallName == "count"
                && dynamic_pointer_cast<CajetaArray>(receiverType)) {
            auto arrayType = dynamic_pointer_cast<CajetaArray>(receiverType);
            llvm::Value* sizePtr = builder->CreateStructGEP(
                arrayType->getLlvmType(), receiver, CajetaArray::SIZE_FIELD_INDEX);
            return builder->CreateLoad(
                llvm::Type::getInt64Ty(*module->getLlvmContext()), sizePtr);
        }

        // P6.6 — `arr.stream()` intrinsic. Lowers to
        // `heap ArrayStream<T>(arr, arr.length())`. The stdlib parse
        // populates the cajeta.lang.stream.ArrayStream template into
        // canonicalMap before user code runs, so template lookup
        // by canonical name succeeds even without an explicit import.
        // Mirrors the count() intrinsic immediately above — same
        // receiver-shape gate (CajetaArray), no params, structural
        // construction inline rather than going through NewExpression
        // (we'd have to fabricate a parsed-AST creator + parameters,
        // which loses the resolved receiver value we already have).
        if (receiver && methodCallName == "stream" && parameters.empty()) {
            if (auto arrayType = dynamic_pointer_cast<CajetaArray>(receiverType)) {
                auto elemType = arrayType->getElementType();
                CajetaTypePtr streamTemplate =
                    CajetaType::of("ArrayStream", "cajeta.lang.stream");
                auto streamKlass = dynamic_pointer_cast<CajetaClass>(streamTemplate);
                if (streamKlass && streamKlass->isTemplate()) {
                    auto instantiated = dynamic_pointer_cast<CajetaClass>(
                        streamKlass->instantiate({elemType}));
                    if (instantiated) {
                        auto& llvmCtx = *module->getLlvmContext();
                        llvm::Type* structTy = instantiated->getLlvmType();
                        const llvm::DataLayout& dl =
                            module->getLlvmModule()->getDataLayout();
                        llvm::Constant* allocSize = llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(llvmCtx),
                            dl.getTypeAllocSize(structTy));
                        llvm::Value* instance = MemoryManager::createMallocInstruction(
                            module, allocSize, builder->GetInsertBlock());
                        builder->CreateMemSet(instance,
                            llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvmCtx), 0),
                            allocSize, llvm::MaybeAlign(8));
                        if (auto* vt = instantiated->getVirtualTableGlobal()) {
                            llvm::Constant* vtableRef = CajetaModule::ensureGlobalInModule(
                                module->getLlvmModule(), vt);
                            llvm::Value* slot = builder->CreateStructGEP(
                                structTy, instance, /*idx=*/0, "vtable_slot");
                            builder->CreateStore(vtableRef, slot);
                        }
                        // Load count from the receiver's i64 size header,
                        // then truncate to i32 (the ctor's limit param).
                        llvm::Value* sizePtr = builder->CreateStructGEP(
                            arrayType->getLlvmType(), receiver,
                            CajetaArray::SIZE_FIELD_INDEX);
                        llvm::Value* sizeI64 = builder->CreateLoad(
                            llvm::Type::getInt64Ty(llvmCtx), sizePtr);
                        llvm::Value* sizeI32 = builder->CreateIntCast(
                            sizeI64, llvm::Type::getInt32Ty(llvmCtx), true);
                        vector<ParameterEntry> entries;
                        // Use the ctor's declared parameter labels so
                        // invokeMethod takes the labeled-lookup path
                        // (ArrayStream's ctor was registered there
                        // since the source declares labels).
                        entries.push_back(ParameterEntry(arrayType, "data", receiver));
                        entries.push_back(ParameterEntry(
                            CajetaType::of("int32"), "limit", sizeI32));
                        string ctorName = "ArrayStream";
                        instantiated->invokeMethod(ctorName, entries,
                            /*isConstructor=*/true, instance, module);
                        // Pin resolvedType so a caller using this MCE as a
                        // ctor / method argument can recover the static type
                        // (ArrayStream<T>) instead of falling back to the
                        // generic `pointer` type CajetaType::of(value)
                        // returns for an opaque-pointer Value. Mirrors what
                        // NewExpression::resolveTypes does for `new T(...)`.
                        resolvedType = instantiated;
                        return instance;
                    }
                }
            }
        }

        // Host-array vectorized load/store — the width-generic parity surface
        // for `Cajeta.vload8f64`/`vstore8f64` (kernel-vector-loadstore-spec.md
        // §7). `arr.vload<N>(i)` / `arr.vstore(i, v)` on a CajetaArray receiver
        // read identically to the kernel-buffer form (only the receiver type
        // differs). The host array header is `{ i64 size, [0 x elem] data }`,
        // so data starts at byte +8 and `i` is an element-stride GEP. T and N
        // are generic: T from the array's element type, N from `<N>` (vload) or
        // the value's vector width (vstore). A packed `<N x T>` memory op at the
        // element's natural alignment (safe for any index) — the legacy
        // fixed-name intrinsics remain as deprecated aliases above.
        if (receiver && (methodCallName == "vload" || methodCallName == "vstore")) {
            if (auto arrayType = dynamic_pointer_cast<CajetaArray>(receiverType)) {
                auto* i8Ty = builder->getInt8Ty();
                CajetaTypePtr elemCT = arrayType->getElementType();
                llvm::Type* elemTy = elemCT->getLlvmType();
                const llvm::DataLayout& dl =
                    module->getLlvmModule()->getDataLayout();
                llvm::Align elemAlign = dl.getABITypeAlign(elemTy);
                auto evalArg = [&](size_t i) -> llvm::Value* {
                    auto& p = parameters[i].expression;
                    llvm::Value* v = p->generateCode(module);
                    auto ast = dynamic_pointer_cast<Expression>(p);
                    if (ast && !ast->getResolvedType()) ast->resolveTypes(module);
                    return loadIfLValue(module, v, ast);
                };

                if (methodCallName == "vload" && parameters.size() == 1
                        && explicitMethodTypeArgs.size() == 1) {
                    auto cN = dynamic_pointer_cast<CajetaConstantType>(
                        explicitMethodTypeArgs[0]);
                    if (cN) {
                        unsigned lanes = (unsigned) cN->getValue();
                        llvm::Value* idx = builder->CreateIntCast(
                            evalArg(0), builder->getInt64Ty(), /*isSigned=*/true);
                        llvm::Value* data = builder->CreateGEP(
                            i8Ty, receiver, builder->getInt64(8), "varr_data");
                        llvm::Value* ptr = builder->CreateGEP(
                            elemTy, data, idx, "vload_ptr");
                        auto* vTy = llvm::FixedVectorType::get(elemTy, lanes);
                        llvm::LoadInst* ld = builder->CreateLoad(vTy, ptr, "vload");
                        ld->setAlignment(elemAlign);
                        resolvedType = CajetaVector::validateAndCreate(
                            module, elemCT, lanes);
                        return ld;
                    }
                }

                if (methodCallName == "vstore" && parameters.size() == 2) {
                    llvm::Value* idx = builder->CreateIntCast(
                        evalArg(0), builder->getInt64Ty(), /*isSigned=*/true);
                    llvm::Value* vec = evalArg(1);
                    if (!vec->getType()->isVectorTy()) {
                        throw Exception(
                            "'vstore' requires a Vector value; got a non-vector "
                            "argument", "CAJETA_ERROR_VSTORE_NOT_VECTOR");
                    }
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, receiver, builder->getInt64(8), "varr_data");
                    llvm::Value* ptr = builder->CreateGEP(
                        elemTy, data, idx, "vstore_ptr");
                    llvm::StoreInst* st = builder->CreateStore(vec, ptr);
                    st->setAlignment(elemAlign);
                    return st;
                }
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

        // Kernel-only operations. `buf.vload<N>(i)` / `buf.vstore(i, v)` are
        // lowered inside an @Kernel body by KernelLowering (a kernel's host body
        // is stubbed at Method::generateCode, so a kernel's vload never reaches
        // here). Reaching this point means they were called from a HOST method,
        // where a KernelBuffer has no host address — reject with a clear
        // kernel-only diagnostic instead of silently emitting nothing.
        // (kernel-vector-loadstore-spec.md §3.1.4, §4.1.4.)
        if ((methodCallName == "vload" || methodCallName == "vstore")
                && targetClass->getQName()
                && targetClass->getQName()->toCanonical().rfind(
                       "cajeta.gpu.KernelBuffer", 0) == 0) {
            throw Exception(
                "'" + methodCallName + "' is a kernel-only operation on a "
                "KernelBuffer and can only be called inside an @Kernel body",
                "CAJETA_ERROR_KERNEL_ONLY_OP");
        }

        // Resolve `this`. For cross-object calls the receiver IS the `this`. For
        // bare calls we look it up from the active method's scope.
        //
        // Guard: only consider scope-resident `this` when the enclosing
        // method is non-static. Scopes can carry stale `this` entries from
        // earlier-generated methods on the stack (parent-chain walk in
        // Scope::getField finds them); without the static-method guard,
        // a static method calling another method on the same class would
        // pick up a foreign method's `this` alloca whose Function is
        // a different LLVM Function — producing a self-referencing load
        // that fails JIT verify with "Instruction does not dominate all
        // uses".
        llvm::Value* thisValue = receiver;
        if (!thisValue) {
            MethodPtr enclosing = module->getCurrentMethod();
            bool inStatic = enclosing
                && enclosing->getModifiers().find(STATIC) != enclosing->getModifiers().end();
            if (!inStatic) {
                FieldPtr thisField = module->getScopeStack().peek()->getField("this");
                if (thisField) {
                    llvm::AllocaInst* thisAlloca = thisField->getOrCreateAllocation();
                    if (thisAlloca) {
                        thisValue = builder->CreateLoad(thisAlloca->getAllocatedType(), thisAlloca);
                    }
                }
            }
        }

        // Lambda-as-arg expectedType propagation. If any arg is a
        // LambdaExpression with no resolvedType yet, look up the
        // target method by name + arg count on targetClass and
        // populate each lambda's expectedType from the matching
        // formal parameter. This lets `s.forEach((T x) -> ...)`
        // resolve the lambda's signature from the method's
        // declared `(T) -> void` parameter rather than falling back
        // to void (LambdaExpression::resolveTypes' default when
        // the body is a block). Targets a single same-name method
        // with matching arg count; if the lookup is ambiguous
        // (overloaded by arity match), no propagation runs and the
        // lambda lands at the default — at which point overload
        // resolution fails downstream the same way it did before.
        // Run the propagator when ANY lambda arg needs help:
        //   - It has no resolvedType yet (typed-param lambdas resolved
        //     post-MCE only).
        //   - OR its paramTypes/paramNames arity doesn't match (a bare-id
        //     lambda whose earlier resolveTypes ran without a parameter
        //     scope, fell back to `() -> void`, and pinned that on the
        //     lambda — `getResolvedType()` returns the stale placeholder
        //     so the original "no resolvedType" gate skipped these).
        bool anyLambda = false;
        for (auto& param : parameters) {
            if (auto lam = std::dynamic_pointer_cast<LambdaExpression>(
                    param.expression)) {
                if (!lam->getResolvedType()
                        || lam->getParamTypes().size() < lam->getParamNames().size()) {
                    anyLambda = true;
                    break;
                }
            }
        }
        if (anyLambda && targetClass) {
            MethodPtr candidate;
            int matches = 0;
            // Walk targetClass AND its parent chain — inherited methods
            // (e.g. Stream<T>'s `forEach` called on ArrayStream<T>) live
            // on the parent and getMethods() returns only the receiver's
            // own declarations. Without walking, the lambda's
            // expectedType doesn't get pinned, the lambda's body type
            // (e.g. an int32 assignment expression) becomes the return
            // type, and the resulting (T) -> int32 doesn't match the
            // parent's (T) -> void signature — the call gets silently
            // dropped at resolveMethod time. First-match wins per
            // override semantics (subclass overrides take precedence
            // over inherited).
            std::function<void(CajetaClassPtr)> findCandidate =
                [&](CajetaClassPtr cls) {
                    if (!cls) return;
                    for (auto& mEntry : cls->getMethods()) {
                        auto& m = mEntry.second;
                        if (m->getName() != methodCallName) continue;
                        // Method-template overloads share a name and
                        // (with explicit type-args) the same type-param
                        // count — `fold<R>(R, (R,T)->R)` and
                        // `fold<R>(R, (R,T)->R, (R,R)->R)` both match
                        // a `<int64>` call. Disambiguate by value-arity
                        // here using the template's source-declared
                        // parameterList. Generic templates skip
                        // generatePrototype's `this`-prepend (Method.cpp
                        // line ~415 returns early for templates), so
                        // the raw parameterList may have no `this`
                        // entry; the propagator below at line ~2080
                        // already uses the same `front()->getName() ==
                        // "this"` detection to handle the instantiated-
                        // mid-build state.
                        bool isMethodTpl = m->isMethodTemplate();
                        if (isMethodTpl && !explicitMethodTypeArgs.empty()
                                && explicitMethodTypeArgs.size()
                                    == m->getMethodTypeParameters().size()) {
                            auto pList = m->getParameterList();
                            bool hasThisTpl = !pList.empty()
                                && pList.front()->getName() == "this";
                            int declaredTpl = (int) pList.size()
                                - (hasThisTpl ? 1 : 0);
                            // Static methods have no `this`; un-
                            // instantiated instance templates also omit
                            // it because generatePrototype skips
                            // template signatures. Either way the
                            // `hasThisTpl` check is the right subtractor.
                            if (declaredTpl != (int) parameters.size()) {
                                continue;
                            }
                            candidate = m;
                            ++matches;
                            continue;
                        }
                        if (isMethodTpl) continue;
                        // Skip method-template instantiations from prior
                        // call sites. Their methodTypeArguments are
                        // already bound to whatever the prior site
                        // resolved (often unrelated — e.g. `Stream<P>
                        // .reduce` pre-instantiates `fold<P>` at
                        // delegation time, and a later
                        // `Stream<P>.fold<int32>(...)` call would
                        // otherwise see both the template AND the stale
                        // `<P>` instantiation as matches, kicking us
                        // out of the matches==1 propagator path).
                        if (m->isMethodTemplateInstantiation()) continue;
                        // A call written with explicit method type-args
                        // (`foo<T>(...)`) selects a method TEMPLATE; a same-name,
                        // same-arity NON-template overload is not a candidate.
                        // Without this both match (matches==2) and the
                        // matches==1 instantiation/lambda-inference path below is
                        // skipped — the same overload collision that mis-pins a
                        // templated call's return type (e.g. Json.parse<Box>).
                        if (!explicitMethodTypeArgs.empty()) continue;
                        bool isStaticM = m->getModifiers().find(STATIC)
                            != m->getModifiers().end();
                        int declared = (int) m->getParameterList().size()
                            - (isStaticM ? 0 : 1);
                        if (declared != (int) parameters.size()) continue;
                        candidate = m;
                        ++matches;
                    }
                    // Only recurse into parents when we haven't found a
                    // matching method here — subclass methods shadow
                    // inherited ones with the same signature shape.
                    if (matches == 0) {
                        for (auto& sup : cls->getSuperClasses()) {
                            findCandidate(sup);
                            if (matches > 0) break;
                        }
                    }
                };
            findCandidate(targetClass);
            // Skip the propagator when the candidate is method-
            // templated (either the unbound template or a concrete
            // instantiation from some other call site). For templates,
            // the formal types carry placeholder T-vars that would
            // propagate as the lambda's expectedType return (a
            // placeholder whose getLlvmType is `ptr`, producing a
            // `ptr (...)` lambda signature). For instantiations, the
            // concrete T-args fit *that* call site but not necessarily
            // ours — `Stream<Counter>.reduce` (which delegates to
            // fold<Counter>) creates an instantiation the propagator
            // would mistakenly pin to our `fold<int32>` call's lambda.
            // The lambda's body-inference path (with the parameter
            // scope pushed in LambdaExpression::resolveTypes) is more
            // reliable here; unification at the call site binds R
            // from the lambda's actual return type.
            // For method-templates, only skip when the call site is
            // relying on inference (no explicit type args). When the
            // user wrote `.map<int32>(...)` the explicit args bind the
            // method's T-vars concretely; instantiate the template
            // right here and let propagation use the bound formal
            // types. Without this, fluent forms like
            // `xs.stream().map<int32>((x) -> x * 10)` failed lambda
            // type inference because the bare-param `x` has no other
            // context to bind from.
            if (candidate && candidate->isMethodTemplate()
                    && !explicitMethodTypeArgs.empty()
                    && explicitMethodTypeArgs.size()
                        == candidate->getMethodTypeParameters().size()) {
                try {
                    candidate = candidate->instantiateMethodTemplate(
                        explicitMethodTypeArgs);
                } catch (...) {
                    candidate = nullptr;
                }
            } else if (candidate
                    && (candidate->isMethodTemplate()
                        || candidate->isMethodTemplateInstantiation())) {
                candidate = nullptr;
            }
            if (candidate && matches == 1) {
                auto paramList = candidate->getParameterList();
                bool isStaticM = candidate->getModifiers().find(STATIC)
                    != candidate->getModifiers().end();
                // Method-template instantiations don't have `this`
                // inserted until LLVM signature build. Detect raw-formals
                // state by checking whether the first param is "this".
                bool hasThis = !paramList.empty()
                    && paramList.front()->getName() == "this";
                int paramOffset = (isStaticM || !hasThis) ? 0 : 1;
                size_t i = 0;
                for (auto& p : paramList) {
                    if ((int) i < paramOffset) { ++i; continue; }
                    size_t argIdx = i - paramOffset;
                    if (argIdx >= parameters.size()) break;
                    if (auto lambda = std::dynamic_pointer_cast<LambdaExpression>(
                            parameters[argIdx].expression)) {
                        // Pin expectedType when the lambda either has no
                        // resolvedType yet OR has unresolved bare-id
                        // params (stale `() -> void` placeholder).
                        bool needsInference = !lambda->getResolvedType()
                            || lambda->getParamTypes().size()
                                < lambda->getParamNames().size();
                        if (needsInference && p->getType()) {
                            lambda->setExpectedType(p->getType());
                        }
                    }
                    ++i;
                }
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
            } else if (llvm::isa_and_nonnull<llvm::GlobalVariable>(value)
                    && dynamic_pointer_cast<IdentifierExpression>(param.expression)) {
                // A bare static-field identifier (`TAG`, the `Main.TAG`
                // shorthand) returns the field's GlobalVariable slot — load
                // through to the value, mirroring the alloca case. Gated on
                // IdentifierExpression so a String / `T.class` literal global
                // (an r-value whose ADDRESS is the value) is left untouched.
                auto* g = llvm::cast<llvm::GlobalVariable>(value);
                value = builder->CreateLoad(g->getValueType(), g);
            }
            // Field reads (DotExpression) on a primitive or function-
            // typed field return an l-value GEP slot. Load through so
            // the call's arg is the field's content. Restricted to
            // DotExpression specifically — loadIfLValue's broader path
            // would mis-load class-typed local Identifiers (whose
            // alloca's allocatedType is the body struct, not the
            // canonical ptr). Without this, `this.fold(c.seed,
            // c.accumulator)` inside `Stream<T>.collect<R>` passes
            // the field-GEPs to fold, mismatching fold's
            // seed:int32 / fn:fn-typed signature at JIT verify.
            //
            // Array element reads (ArrayIndexExpression, `arr[i]`) have the
            // same shape: generateCode returns the element GEP slot, so a bare
            // `f(arr[i])` would pass the element ADDRESS (ptr) where the value
            // is wanted — a JIT verify failure ("Call parameter type does not
            // match function signature!") for a primitive element. loadIfLValue's
            // ArrayIndexExpression branch loads primitives to their value and
            // leaves reference/interface elements as the (correct) pointer/body,
            // so it's safe to apply here too. (Previously only a named-local or
            // arithmetic-wrapped element worked; `f(arr[i])` directly did not.)
            if (dynamic_pointer_cast<DotExpression>(param.expression)
                    || dynamic_pointer_cast<ArrayIndexExpression>(param.expression)) {
                value = loadIfLValue(module, value, param.expression);
            }
            CajetaTypePtr et = param.expression->getResolvedType();
            if (!et) et = CajetaType::of(value);
            // Capture identity (P2-2 item 1): if the argument is a
            // method call on the SAME identifier-named receiver as
            // this outer call, prefer the un-projected wildcard return
            // type as the entry type. That way subtypeDistance sees
            // wildcard-vs-wildcard (canonical match) and the read-back
            // pattern `b.set(b.get())` resolves cleanly. A foreign
            // Animal arg lacks this match and continues to be rejected
            // (PECS soundness preserved).
            //
            // v1 scope: identifier-receiver match only. Chained or
            // `this`-based receivers fall back to the projected type
            // and reject — a deeper capture model would generalize.
            if (!children.empty()) {
                auto outerRecvId = dynamic_pointer_cast<IdentifierExpression>(children[0]);
                auto argMce = dynamic_pointer_cast<MethodCallExpression>(param.expression);
                if (outerRecvId && argMce
                        && argMce->getPreProjectionReturnType()) {
                    auto& argChildren = argMce->getChildren();
                    if (!argChildren.empty()) {
                        auto argRecvId = dynamic_pointer_cast<IdentifierExpression>(argChildren[0]);
                        if (argRecvId
                                && argRecvId->getTextValue()
                                    == outerRecvId->getTextValue()) {
                            et = argMce->getPreProjectionReturnType();
                        }
                    }
                }
            }
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
        // Pass the active codegen `module`: this advisory lint resolve runs during
        // generateCode and can INSTANTIATE a method template whose T is inferable
        // from the value args (e.g. `Protobuf.toBytes<T>(T value)`), which brings
        // it to life here. Without the active module, the insert-point save/restore
        // in bringMethodTemplateInstantiationToLife falls back to the emit module's
        // (dangling) builder → freed-builder SEGV. See memory
        // method-template-tparam-in-param-heapcorrupt.
        MethodPtr targetMethod = targetClass->resolveMethod(
            methodCallName, entriesCopy, /*isConstructor=*/false,
            floatingParamsLint, {}, module);
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

            // wildcard-materialize-in-loop (docs/LintRules.md).
            // An element-producing call on a wildcard-typed receiver
            // inside a loop body forces template-relative vtable
            // dispatch on every iteration — the inliner can't see
            // through it and LLVM loses the unroll/vectorize window.
            // Trigger: hasLoopContext + receiver is a wildcard
            // instantiation + method name is in the element-producing
            // set (next / get). Suppressible per enclosing method via
            // @SuppressLint("wildcard-materialize-in-loop").
            //
            // wildcard-crosses-hot-boundary (docs/LintRules.md).
            // A wildcard-return call inside a loop pays a downcast at
            // every receive site that wants the concrete type, and
            // the call itself can't be inline-specialized. Trigger:
            // hasLoopContext + targetMethod's return type is a
            // wildcard instantiation. Suppressible via
            // @SuppressLint("wildcard-crosses-hot-boundary").
            auto currentMethod = module->getCurrentMethod();
            // Skip both wildcard lints when the enclosing context is a
            // compiler artifact rather than user-authored code:
            //   - Wildcard-proxy class: the body is generated for the
            //     erased instantiation; the loop/receiver/return shape
            //     is whatever the template says with T → ?.
            //   - Method-level template instantiation: T inside the body
            //     can resolve to the wildcard sentinel as a stand-in
            //     because the type cache uses one sentinel for both
            //     "real `?`" and "uninstantiated T placeholder". Without
            //     capture conversion (P2-2 item 1) we can't distinguish,
            //     so suppress the lint here to avoid false positives.
            bool enclosingIsWildcardProxy = currentMethod
                && currentMethod->getParent()
                && currentMethod->getParent()->isWildcardInstantiation();
            bool enclosingIsMethodTemplate = currentMethod
                && (currentMethod->isMethodTemplate()
                    || currentMethod->isMethodTemplateInstantiation());
            if (currentMethod && module->hasLoopContext()
                    && !enclosingIsWildcardProxy
                    && !enclosingIsMethodTemplate) {
                bool receiverIsWildcard = targetClass
                    && targetClass->isWildcardInstantiation();
                bool isElementProducing =
                    methodCallName == "next" || methodCallName == "get";
                if (receiverIsWildcard && isElementProducing
                        && !currentMethod->isLintSuppressed(
                            "wildcard-materialize-in-loop")) {
                    std::cerr << "warning: [wildcard-materialize-in-loop] "
                        << "call to '" << methodCallName
                        << "' on wildcard-typed receiver "
                        << targetClass->toCanonical()
                        << " inside a loop in "
                        << currentMethod->getName()
                        << " — dispatch goes through the template-relative "
                        << "vtable hash on every iteration; downcast to the "
                        << "concrete type at the loop boundary, or suppress "
                        << "with @SuppressLint(\"wildcard-materialize-in-loop\")."
                        << std::endl;
                }
                if (targetMethod
                        && !currentMethod->isLintSuppressed(
                            "wildcard-crosses-hot-boundary")) {
                    auto retClass = dynamic_pointer_cast<CajetaClass>(
                        targetMethod->getReturnType());
                    if (retClass && retClass->isWildcardInstantiation()) {
                        std::cerr << "warning: [wildcard-crosses-hot-boundary] "
                            << "call to '" << methodCallName
                            << "' inside a loop in "
                            << currentMethod->getName()
                            << " returns wildcard-typed "
                            << retClass->toCanonical()
                            << " — the receive site can't be specialized "
                            << "at the call boundary; restructure to "
                            << "concrete or suppress with "
                            << "@SuppressLint(\"wildcard-crosses-hot-boundary\")."
                            << std::endl;
                    }
                }
            }
        }

        // `super.method()` — bypass vtable dispatch and direct-call the
        // parent's body. Without this, the instance's vtable (which
        // belongs to the most-derived class) would route back to the
        // override and infinite-loop. SuperExpression as the receiver
        // child is the trigger; SuperExpression::resolveTypes set the
        // receiverType to the first declared parent, so targetClass
        // above is already the right class for resolution.
        std::shared_ptr<SuperExpression> superLhs;
        if (!children.empty()) {
            superLhs = std::dynamic_pointer_cast<SuperExpression>(children[0]);
        }
        bool isSuperCall = (superLhs != nullptr);

        // MultiClassing Phase 3 v3 (docs/specification/lang/MultiClassing.md
        // § P-4): inherited-method re-adjustment for diamond. When the
        // user writes `super<C>.method()` and `method` is INHERITED from
        // an ancestor A (not declared on C itself), the dispatch lands
        // on A's standalone function — which expects `this` to be an
        // A-pointer. SuperExpression already adjusted `this` to C's
        // sub-object; if C's standalone layout has A inline at the same
        // relative offset (single inheritance, no diamond), that
        // adjustment naturally lines up. In a diamond, though, A's
        // canonical position in the enclosing class is NOT reachable by
        // GEPing through C's standalone struct type — C's inline-A is
        // dormant.
        //
        // The fix: when isSuperCall, the bracketed class differs from
        // the method's declaring class, and a diamond is detected, shift
        // `thisValue` from the bracketed position to the declaring
        // class's canonical position in the enclosing class. Same
        // detection formula as DotExpression's Phase 3 v2 routing:
        //   canonical = enclosing.getSubObjectByteOffset(declaringClass)
        //   via_brkt  = enclosing.getSubObjectByteOffset(bracketed)
        //             + bracketed.getSubObjectByteOffset(declaringClass)
        // when they differ, delta = canonical - via_brkt is applied to
        // `thisValue`.
        //
        // Out of scope for v3 (would need vbase ABI or per-descendant
        // recompilation): when the non-first parent C has its OWN
        // method (declared on C, not inherited) that internally touches
        // a shared ancestor's fields via `this.x`. In that case the
        // declaring class equals the bracketed class, so no re-adjust
        // fires; C's IR runs with C-adjusted `this` and GEPs land on
        // C's dormant inline-A. Tracked as the v4 follow-up.
        if (isSuperCall && superLhs && !superLhs->getChosenAncestorName().empty()
                && thisValue && !module->getStructureStack().empty()) {
            auto bracketed = std::dynamic_pointer_cast<CajetaClass>(
                superLhs->getResolvedType());
            auto enclosing = std::dynamic_pointer_cast<CajetaClass>(
                module->getStructureStack().back());
            if (bracketed && enclosing && targetClass) {
                bool callFloating = true;
                for (auto& e : entries) {
                    if (e.label.empty()) { callFloating = false; break; }
                }
                MethodPtr resolved = targetClass->resolveMethod(
                    methodCallName, entries,
                    /*isConstructor=*/false, callFloating);
                CajetaClassPtr declaringClass;
                if (resolved) {
                    declaringClass = resolved->getParent();
                }
                if (declaringClass && declaringClass.get() != bracketed.get()) {
                    uint64_t canonical = enclosing->getSubObjectByteOffset(
                        declaringClass.get());
                    uint64_t viaBrkt = enclosing->getSubObjectByteOffset(
                            bracketed.get())
                        + bracketed->getSubObjectByteOffset(
                            declaringClass.get());
                    if (canonical != viaBrkt) {
                        auto& ctx = *module->getLlvmContext();
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
                        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
                        int64_t delta =
                            (int64_t) canonical - (int64_t) viaBrkt;
                        thisValue = builder->CreateInBoundsGEP(i8Ty,
                            thisValue,
                            llvm::ConstantInt::get(i64Ty, delta),
                            "diamond_super_canonical");
                    }
                }
                // Phase 3 v4 full vbase ABI now handles "method declared
                // on bracketed class that touches inherited fields" via
                // vbase indirection in DotExpression. No `this`
                // adjustment needed at the call site.
            }
        }

        // ----- Path instance-method stat intrinsics (Phase C) -----
        // The cajeta-side bodies are stubs; here we lower
        // exists / isFile / isDir / isSymlink to direct
        // `__cajeta_path_*` runtime helper calls. The runtime
        // helpers take (bytes, length) — the Path's int8[] data
        // ptr (GEP'd past the 8-byte CajetaArray count word) and
        // the byte length.
        if (thisValue && targetClass && targetClass->getQName()
                && targetClass->getQName()->toCanonical() == "cajeta.io.file.Path") {
            auto* pathStructTy = llvm::cast<llvm::StructType>(
                targetClass->getLlvmType());
            llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
            llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);
            // Path layout: { vtable@0, bytes@1 }. The bytes field
            // holds a ptr to the CajetaArray header.
            auto loadBytesAndLen = [&]() -> std::pair<llvm::Value*, llvm::Value*> {
                llvm::Value* bytesSlot = builder->CreateStructGEP(
                    pathStructTy, thisValue, 1, "path.bytes_slot");
                llvm::Value* arrPtr = builder->CreateLoad(
                    ptrTy, bytesSlot, "path.arr");
                // Length is the first i64 of the header.
                llvm::Value* len = builder->CreateLoad(
                    i64Ty, arrPtr, "path.len");
                // Data starts at offset 8.
                llvm::Value* data = builder->CreateInBoundsGEP(
                    i8Ty, arrPtr,
                    llvm::ConstantInt::get(i64Ty, 8),
                    "path.data");
                return {data, len};
            };

            const char* statSymbol = nullptr;
            if (methodCallName == "exists" && parameters.empty()) {
                statSymbol = "__cajeta_path_exists";
            } else if (methodCallName == "isFile" && parameters.empty()) {
                statSymbol = "__cajeta_path_is_file";
            } else if (methodCallName == "isDir" && parameters.empty()) {
                statSymbol = "__cajeta_path_is_dir";
            } else if (methodCallName == "isSymlink" && parameters.empty()) {
                statSymbol = "__cajeta_path_is_symlink";
            }
            if (statSymbol) {
                llvm::Function* fn = module->getRuntimeFunction(statSymbol);
                if (fn) {
                    auto bd = loadBytesAndLen();
                    llvm::Value* result = builder->CreateCall(fn,
                        {bd.first, bd.second}, "path.stat");
                    // The helper returns int32 (1/0); the cajeta
                    // method signature is `boolean` (i1). Truncate
                    // / icmp to widen to the right shape.
                    llvm::Value* asI1 = builder->CreateICmpNE(result,
                        llvm::ConstantInt::get(i32Ty, 0),
                        "path.stat.bool");
                    resolvedType = CajetaType::of("boolean");
                    return asI1;
                }
            }

            // Phase D mutators: mkdirs / delete. The runtime
            // helpers return int32 (0/-1). Today we ignore the
            // failure return; once the IoException hierarchy is
            // wired end-to-end, codegen branches to a throw on
            // the -1 path.
            if (methodCallName == "mkdirs" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_mkdirs");
                if (fn) {
                    auto bd = loadBytesAndLen();
                    builder->CreateCall(fn, {bd.first, bd.second});
                    resolvedType = targetClass;
                    return thisValue;  // chaining: returns the Path.
                }
            }
            if (methodCallName == "delete" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_delete");
                if (fn) {
                    auto bd = loadBytesAndLen();
                    builder->CreateCall(fn, {bd.first, bd.second});
                    resolvedType = CajetaType::of("void");
                    return nullptr;
                }
            }

            // setExecutable() — chmod a+x on the file (preserving other
            // bits), so a freshly written/downloaded binary becomes
            // runnable. Runtime helper returns int32 0/-1; the cajeta
            // signature is `boolean` (true on success).
            if (methodCallName == "setExecutable" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_set_executable");
                if (fn) {
                    auto bd = loadBytesAndLen();
                    llvm::Value* result = builder->CreateCall(fn,
                        {bd.first, bd.second}, "path.setx");
                    llvm::Value* ok = builder->CreateICmpEQ(result,
                        llvm::ConstantInt::get(i32Ty, 0), "path.setx.ok");
                    resolvedType = CajetaType::of("boolean");
                    return ok;
                }
            }

            // symlinkTo(Path target) — create `this` as a symlink pointing
            // at `target` (ln -s target this). cvm repoints the active
            // toolchain shim this way. `this` is the link location; the
            // argument Path is the target. Runtime helper returns 0/-1.
            if (methodCallName == "symlinkTo" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_symlink");
                if (fn) {
                    // Extract (data, len) from the argument Path the same
                    // way loadBytesAndLen() does for `this`.
                    llvm::Value* targetPtr = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* tBytesSlot = builder->CreateStructGEP(
                        pathStructTy, targetPtr, 1, "tgt.bytes_slot");
                    llvm::Value* tArrPtr = builder->CreateLoad(
                        ptrTy, tBytesSlot, "tgt.arr");
                    llvm::Value* tLen = builder->CreateLoad(
                        i64Ty, tArrPtr, "tgt.len");
                    llvm::Value* tData = builder->CreateInBoundsGEP(
                        i8Ty, tArrPtr,
                        llvm::ConstantInt::get(i64Ty, 8), "tgt.data");
                    auto link = loadBytesAndLen();  // this = link location
                    llvm::Value* result = builder->CreateCall(fn,
                        {tData, tLen, link.first, link.second},
                        "path.symlink");
                    llvm::Value* ok = builder->CreateICmpEQ(result,
                        llvm::ConstantInt::get(i32Ty, 0), "path.symlink.ok");
                    resolvedType = CajetaType::of("boolean");
                    return ok;
                }
            }

            // canonical() — returns a fresh Path wrapping the
            // realpath result. The runtime hands back a
            // CajetaArray header for the bytes; we wrap that in a
            // fresh Path struct here (vtable + bytes ptr).
            if (methodCallName == "canonical" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_canonical");
                if (fn) {
                    auto bd = loadBytesAndLen();
                    llvm::Value* canonBytes = builder->CreateCall(fn,
                        {bd.first, bd.second}, "path.canon_arr");
                    // Allocate a new Path struct, set vtable + bytes,
                    // return it.
                    const llvm::DataLayout& dl =
                        module->getLlvmModule()->getDataLayout();
                    llvm::Constant* size = llvm::ConstantInt::get(
                        i64Ty, dl.getTypeAllocSize(pathStructTy));
                    llvm::Value* inst = MemoryManager::createMallocInstruction(
                        module, size, builder->GetInsertBlock());
                    builder->CreateMemSet(inst,
                        llvm::ConstantInt::get(i8Ty, 0),
                        size, llvm::MaybeAlign(8));
                    llvm::Constant* vtableRef =
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy));
                    if (auto* vt = targetClass->getVirtualTableGlobal()) {
                        vtableRef = CajetaModule::ensureGlobalInModule(
                            module->getLlvmModule(), vt);
                    }
                    builder->CreateStore(vtableRef,
                        builder->CreateStructGEP(pathStructTy, inst, 0,
                            "path.canon.vtable_slot"));
                    builder->CreateStore(canonBytes,
                        builder->CreateStructGEP(pathStructTy, inst, 1,
                            "path.canon.bytes_slot"));
                    resolvedType = targetClass;
                    return inst;
                }
            }
        }

        // ----- FileReader / FileWriter / File instance-method intrinsic -----
        // Phase A: FileReader / FileWriter (streaming).
        // Phase E: File (random-access — seek / lock / truncate / sync).
        //
        // The cajeta-side bodies are stubs; we lower calls to direct
        // runtime helpers via the receiver's `fd` field. Matches the
        // spec in docs/specification/io/file/{FileReader,FileWriter,
        // File}.md.
        if (thisValue && targetClass && targetClass->getQName()) {
            const std::string canonical = targetClass->getQName()->toCanonical();
            bool isReader = canonical == "cajeta.io.file.FileReader";
            bool isWriter = canonical == "cajeta.io.file.FileWriter";
            bool isFile   = canonical == "cajeta.io.file.File";
            if (isReader || isWriter || isFile) {
                auto* structTy =
                    llvm::cast<llvm::StructType>(targetClass->getLlvmType());
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);
                // Field offsets: vtable(0), fd(1), pos(2). Pinned by
                // FileReader.cajeta / FileWriter.cajeta field order.
                auto loadFd = [&]() -> llvm::Value* {
                    llvm::Value* fdSlot = builder->CreateStructGEP(
                        structTy, thisValue, 1, "fr.fd_slot");
                    return builder->CreateLoad(i32Ty, fdSlot, "fr.fd");
                };

                if (isReader && methodCallName == "read"
                        && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_file_read");
                    if (fn) {
                        llvm::Value* fd = loadFd();
                        llvm::Value* arr = parameters[0].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(arr)) {
                            arr = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                            i8Ty, arr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "fr.buf");
                        llvm::Value* maxV = parameters[1].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(maxV)) {
                            maxV = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        if (maxV && maxV->getType() != i32Ty
                                && maxV->getType()->isIntegerTy()) {
                            maxV = builder->CreateIntCast(maxV, i32Ty, true);
                        }
                        llvm::Value* nRead = builder->CreateCall(fn,
                            {fd, dataPtr, maxV}, "fr.n");
                        // Update this.pos += (int64) nRead.
                        llvm::Value* posSlot = builder->CreateStructGEP(
                            structTy, thisValue, 2, "fr.pos_slot");
                        llvm::Value* curPos = builder->CreateLoad(
                            i64Ty, posSlot, "fr.pos_cur");
                        llvm::Value* nRead64 = builder->CreateIntCast(
                            nRead, i64Ty, /*isSigned=*/true);
                        llvm::Value* newPos = builder->CreateAdd(
                            curPos, nRead64, "fr.pos_new");
                        builder->CreateStore(newPos, posSlot);
                        resolvedType = CajetaType::of("int32");
                        return nRead;
                    }
                }
                // FileReader.readString(maxBytes) → String. Allocates
                // a fresh int8[maxBytes], reads into it, shrinks the
                // count word to the actual byte count, wraps in a
                // class String shell (mode=0 owned), and returns it.
                if (isReader && methodCallName == "readString"
                        && parameters.size() == 1) {
                    llvm::Function* readFn = module->getRuntimeFunction(
                        "__cajeta_file_read");
                    if (readFn) {
                        llvm::Value* fd = loadFd();
                        llvm::Value* maxV = parameters[0].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(maxV)) {
                            maxV = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        if (maxV && maxV->getType() != i32Ty
                                && maxV->getType()->isIntegerTy()) {
                            maxV = builder->CreateIntCast(maxV, i32Ty, true);
                        }
                        llvm::Value* maxI64 = builder->CreateIntCast(
                            maxV, i64Ty, true);
                        // Allocate int8[maxBytes] via runtime helper.
                        llvm::Function* allocFn = module->getRuntimeFunction(
                            "__cajeta_new_array_header");
                        llvm::Value* arr = builder->CreateCall(allocFn,
                            { llvm::ConstantInt::get(i64Ty, 8),
                              llvm::ConstantInt::get(i64Ty, 1),
                              maxI64 },
                            "fr.str.arr");
                        llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                            i8Ty, arr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "fr.str.data");
                        llvm::Value* nRead = builder->CreateCall(readFn,
                            {fd, dataPtr, maxV}, "fr.str.n");
                        // Shrink the count word to nRead (sign-extend to i64).
                        llvm::Value* nReadI64 = builder->CreateIntCast(
                            nRead, i64Ty, true);
                        builder->CreateStore(nReadI64, arr);
                        // Update this.pos += nRead.
                        llvm::Value* posSlot = builder->CreateStructGEP(
                            structTy, thisValue, 2, "fr.str.pos_slot");
                        llvm::Value* curPos = builder->CreateLoad(
                            i64Ty, posSlot, "fr.str.pos_cur");
                        llvm::Value* newPos = builder->CreateAdd(
                            curPos, nReadI64, "fr.str.pos_new");
                        builder->CreateStore(newPos, posSlot);
                        // Wrap into a class String shell.
                        CajetaTypePtr stringTy = CajetaType::of("String");
                        auto stringKlass = std::dynamic_pointer_cast<CajetaClass>(stringTy);
                        if (stringKlass && stringKlass->getLlvmType()
                                && llvm::isa<llvm::StructType>(stringKlass->getLlvmType())) {
                            auto* sStructTy = llvm::cast<llvm::StructType>(
                                stringKlass->getLlvmType());
                            const llvm::DataLayout& dl =
                                module->getLlvmModule()->getDataLayout();
                            llvm::Constant* sSize = llvm::ConstantInt::get(
                                i64Ty, dl.getTypeAllocSize(sStructTy));
                            llvm::Value* sInst = MemoryManager::createMallocInstruction(
                                module, sSize, builder->GetInsertBlock());
                            builder->CreateMemSet(sInst,
                                llvm::ConstantInt::get(i8Ty, 0),
                                sSize, llvm::MaybeAlign(8));
                            llvm::Constant* vtableRef =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            if (auto* vt = stringKlass->getVirtualTableGlobal()) {
                                vtableRef = CajetaModule::ensureGlobalInModule(
                                    module->getLlvmModule(), vt);
                            }
                            builder->CreateStore(vtableRef,
                                builder->CreateStructGEP(sStructTy, sInst, 0,
                                    "fr.str.s_vtable"));
                            builder->CreateStore(arr,
                                builder->CreateStructGEP(sStructTy, sInst, 1,
                                    "fr.str.s_bytes"));
                            // byteLength as i32.
                            llvm::Value* lenI32 = builder->CreateIntCast(
                                nRead, i32Ty, true);
                            builder->CreateStore(lenI32,
                                builder->CreateStructGEP(sStructTy, sInst, 2,
                                    "fr.str.s_byteLength"));
                            // mode = 0 (owned).
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, 0),
                                builder->CreateStructGEP(sStructTy, sInst, 3,
                                    "fr.str.s_mode"));
                            // cachedCpLength = -1 (not computed).
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, -1),
                                builder->CreateStructGEP(sStructTy, sInst, 4,
                                    "fr.str.s_cachedCp"));
                            resolvedType = stringKlass;
                            return sInst;
                        }
                        return arr;
                    }
                }
                if (isReader && methodCallName == "position"
                        && parameters.empty()) {
                    llvm::Value* posSlot = builder->CreateStructGEP(
                        structTy, thisValue, 2, "fr.pos_slot");
                    llvm::Value* pos = builder->CreateLoad(
                        i64Ty, posSlot, "fr.pos");
                    resolvedType = CajetaType::of("int64");
                    return pos;
                }
                if ((isReader || isWriter || isFile) && methodCallName == "close"
                        && parameters.empty()) {
                    // Optionally flush the writer first.
                    if (isWriter) {
                        if (llvm::Function* flushFn = module->getRuntimeFunction(
                                "__cajeta_file_flush")) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(flushFn, {fd});
                        }
                    }
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_file_close");
                    if (fn) {
                        llvm::Value* fd = loadFd();
                        builder->CreateCall(fn, {fd});
                        // Set this.fd = -1 (idempotency: future close()
                        // is a no-op when the runtime helper sees fd < 0).
                        llvm::Value* fdSlot = builder->CreateStructGEP(
                            structTy, thisValue, 1, "fr.fd_slot");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, -1), fdSlot);
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                if (isWriter && methodCallName == "write"
                        && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_file_write");
                    if (fn) {
                        llvm::Value* fd = loadFd();
                        llvm::Value* arr = parameters[0].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(arr)) {
                            arr = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                            i8Ty, arr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "fw.data");
                        llvm::Value* lenV = parameters[1].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(lenV)) {
                            lenV = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        if (lenV && lenV->getType() != i32Ty
                                && lenV->getType()->isIntegerTy()) {
                            lenV = builder->CreateIntCast(lenV, i32Ty, true);
                        }
                        builder->CreateCall(fn, {fd, dataPtr, lenV});
                        // Update this.pos += (int64) len.
                        llvm::Value* posSlot = builder->CreateStructGEP(
                            structTy, thisValue, 2, "fw.pos_slot");
                        llvm::Value* curPos = builder->CreateLoad(
                            i64Ty, posSlot, "fw.pos_cur");
                        llvm::Value* len64 = builder->CreateIntCast(
                            lenV, i64Ty, /*isSigned=*/true);
                        llvm::Value* newPos = builder->CreateAdd(
                            curPos, len64, "fw.pos_new");
                        builder->CreateStore(newPos, posSlot);
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                // FileWriter.writeString(String s) — unwrap the
                // class String to its underlying bytes + byteLength
                // and call __cajeta_file_write.
                if (isWriter && methodCallName == "writeString"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_file_write");
                    if (fn) {
                        llvm::Value* fd = loadFd();
                        llvm::Value* sPtr = parameters[0].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(sPtr)) {
                            sPtr = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        // The class String layout: { vtable@0, bytes@1,
                        // byteLength@2, mode@3, cachedCp@4 }.
                        CajetaTypePtr stringTy = CajetaType::of("String");
                        auto stringKlass = std::dynamic_pointer_cast<CajetaClass>(stringTy);
                        if (stringKlass && stringKlass->getLlvmType()
                                && llvm::isa<llvm::StructType>(stringKlass->getLlvmType())) {
                            auto* sStructTy = llvm::cast<llvm::StructType>(
                                stringKlass->getLlvmType());
                            llvm::Value* bytesSlot = builder->CreateStructGEP(
                                sStructTy, sPtr, 1, "fw.str.bytes_slot");
                            llvm::Value* arr = builder->CreateLoad(
                                ptrTy, bytesSlot, "fw.str.arr");
                            llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                                i8Ty, arr,
                                llvm::ConstantInt::get(i64Ty, 8),
                                "fw.str.data");
                            llvm::Value* byteLenSlot = builder->CreateStructGEP(
                                sStructTy, sPtr, 2, "fw.str.byteLength_slot");
                            llvm::Value* len = builder->CreateLoad(
                                i32Ty, byteLenSlot, "fw.str.len");
                            builder->CreateCall(fn, {fd, dataPtr, len});
                            // Update this.pos += len.
                            llvm::Value* posSlot = builder->CreateStructGEP(
                                structTy, thisValue, 2, "fw.str.pos_slot");
                            llvm::Value* curPos = builder->CreateLoad(
                                i64Ty, posSlot, "fw.str.pos_cur");
                            llvm::Value* len64 = builder->CreateIntCast(
                                len, i64Ty, true);
                            llvm::Value* newPos = builder->CreateAdd(
                                curPos, len64, "fw.str.pos_new");
                            builder->CreateStore(newPos, posSlot);
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                }
                if (isWriter && methodCallName == "flush"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_file_flush");
                    if (fn) {
                        llvm::Value* fd = loadFd();
                        builder->CreateCall(fn, {fd});
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }

                // ----- File random-access instance methods (Phase E) -----
                if (isFile) {
                    // read(dst, offset, length) / write(data, offset, length)
                    if ((methodCallName == "read" || methodCallName == "write")
                            && parameters.size() == 3) {
                        const char* rtSym = methodCallName == "read"
                            ? "__cajeta_file_read"
                            : "__cajeta_file_write";
                        llvm::Function* fn = module->getRuntimeFunction(rtSym);
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* arr = parameters[0].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(arr)) {
                                arr = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            llvm::Value* offV = parameters[1].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(offV)) {
                                offV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (offV && offV->getType() != i64Ty
                                    && offV->getType()->isIntegerTy()) {
                                offV = builder->CreateIntCast(offV, i64Ty, true);
                            }
                            llvm::Value* lenV = parameters[2].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(lenV)) {
                                lenV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (lenV && lenV->getType() != i64Ty
                                    && lenV->getType()->isIntegerTy()) {
                                lenV = builder->CreateIntCast(lenV, i64Ty, true);
                            }
                            // dataPtr = &arr[8 + offset] (count word + offset bytes).
                            llvm::Value* dataStart = builder->CreateInBoundsGEP(
                                i8Ty, arr,
                                llvm::ConstantInt::get(i64Ty, 8),
                                "file.data_start");
                            llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                                i8Ty, dataStart, offV, "file.data_off");
                            // Runtime helpers take int32 length — cast.
                            llvm::Value* len32 = builder->CreateIntCast(
                                lenV, i32Ty, true, "file.len32");
                            llvm::Value* result = builder->CreateCall(fn,
                                {fd, dataPtr, len32}, "file.rw");
                            // Update this.pos += (read ? returned : len).
                            llvm::Value* delta = methodCallName == "read"
                                ? builder->CreateIntCast(result, i64Ty, true)
                                : lenV;
                            llvm::Value* posSlot = builder->CreateStructGEP(
                                structTy, thisValue, 2, "file.pos_slot");
                            llvm::Value* curPos = builder->CreateLoad(
                                i64Ty, posSlot, "file.pos_cur");
                            llvm::Value* newPos = builder->CreateAdd(
                                curPos, delta, "file.pos_new");
                            builder->CreateStore(newPos, posSlot);
                            // Method return is int64.
                            llvm::Value* result64 = builder->CreateIntCast(
                                result, i64Ty, true);
                            resolvedType = CajetaType::of("int64");
                            return result64;
                        }
                    }
                    if (methodCallName == "position" && parameters.empty()) {
                        llvm::Value* posSlot = builder->CreateStructGEP(
                            structTy, thisValue, 2, "file.pos_slot");
                        resolvedType = CajetaType::of("int64");
                        return builder->CreateLoad(i64Ty, posSlot, "file.pos");
                    }
                    if (methodCallName == "seek" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_seek");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* absV = parameters[0].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(absV)) {
                                absV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (absV && absV->getType() != i64Ty
                                    && absV->getType()->isIntegerTy()) {
                                absV = builder->CreateIntCast(absV, i64Ty, true);
                            }
                            // whence = 0 (SEEK_SET).
                            llvm::Value* newPos = builder->CreateCall(fn,
                                {fd, absV, llvm::ConstantInt::get(i32Ty, 0)},
                                "file.seek");
                            llvm::Value* posSlot = builder->CreateStructGEP(
                                structTy, thisValue, 2, "file.pos_slot");
                            builder->CreateStore(newPos, posSlot);
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "seekFromEnd" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_seek");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* offV = parameters[0].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(offV)) {
                                offV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (offV && offV->getType() != i64Ty
                                    && offV->getType()->isIntegerTy()) {
                                offV = builder->CreateIntCast(offV, i64Ty, true);
                            }
                            // whence = 2 (SEEK_END).
                            llvm::Value* newPos = builder->CreateCall(fn,
                                {fd, offV, llvm::ConstantInt::get(i32Ty, 2)},
                                "file.seek_end");
                            llvm::Value* posSlot = builder->CreateStructGEP(
                                structTy, thisValue, 2, "file.pos_slot");
                            builder->CreateStore(newPos, posSlot);
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "size" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_size_of");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            resolvedType = CajetaType::of("int64");
                            return builder->CreateCall(fn, {fd}, "file.size");
                        }
                    }
                    if (methodCallName == "truncate" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_truncate");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* szV = parameters[0].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(szV)) {
                                szV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (szV && szV->getType() != i64Ty
                                    && szV->getType()->isIntegerTy()) {
                                szV = builder->CreateIntCast(szV, i64Ty, true);
                            }
                            builder->CreateCall(fn, {fd, szV});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "sync" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_sync");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(fn, {fd});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "flush" && parameters.empty()) {
                        // No-op for random-access File — but emit
                        // call to the flush helper for consistency.
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_flush");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(fn, {fd});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "lock" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_lock");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(fn, {fd});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "tryLock" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_try_lock");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* result = builder->CreateCall(fn,
                                {fd}, "file.trylock");
                            llvm::Value* asI1 = builder->CreateICmpNE(result,
                                llvm::ConstantInt::get(i32Ty, 0),
                                "file.trylock.bool");
                            resolvedType = CajetaType::of("boolean");
                            return asI1;
                        }
                    }
                    if (methodCallName == "unlock" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_unlock");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(fn, {fd});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                }
            }
        }

        // ----- cajeta.process.Command.run() intrinsic (cajeta-process U1) -----
        //
        // Lowers `Command.run()` to a single `__cajeta_proc_run` call. The C
        // bridge does all marshalling and BUILDS the ProcessResult object,
        // handed (a) the String class's stride + bytes/byteLength offsets so it
        // can read the argv/env String[] and cwd String, and (b) the
        // ProcessResult class's vtable + every field offset (DataLayout) so it
        // can populate the result — mirroring __cajeta_args_make. Command field
        // ABI: argv@1, cwd@2, env@3, stdinData@4, stdinLen@5, stdioFlags@6,
        // timeoutMs@7 (see Command.cajeta).
        if (thisValue && targetClass && targetClass->getQName()) {
            const std::string procCanonical = targetClass->getQName()->toCanonical();
            if (procCanonical == "cajeta.process.Command"
                    && methodCallName == "run" && parameters.empty()) {
                llvm::Function* runFn =
                    module->getRuntimeFunction("__cajeta_proc_run");
                auto* cmdStructTy =
                    llvm::dyn_cast<llvm::StructType>(targetClass->getLlvmType());
                auto& cmap = CajetaType::getCanonicalMap();
                CajetaClassPtr strClass;
                {
                    auto it = cmap.find("cajeta.lang.String");
                    if (it == cmap.end()) it = cmap.find("String");
                    if (it != cmap.end())
                        strClass = std::dynamic_pointer_cast<CajetaClass>(it->second);
                }
                CajetaClassPtr prClass;
                {
                    auto it = cmap.find("cajeta.process.ProcessResult");
                    if (it == cmap.end()) it = cmap.find("ProcessResult");
                    if (it != cmap.end())
                        prClass = std::dynamic_pointer_cast<CajetaClass>(it->second);
                }
                auto* strStructTy = strClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(strClass->getLlvmType())
                    : nullptr;
                auto* prStructTy = prClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(prClass->getLlvmType())
                    : nullptr;
                if (runFn && cmdStructTy && strStructTy && prStructTy) {
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    auto i64c = [&](uint64_t v) {
                        return llvm::ConstantInt::get(i64Ty, v);
                    };
                    auto loadPtrField = [&](unsigned idx, const char* n) {
                        llvm::Value* slot = builder->CreateStructGEP(
                            cmdStructTy, thisValue, idx, n);
                        return builder->CreateLoad(ptrTy, slot, n);
                    };
                    llvm::Value* argvArr = loadPtrField(1, "cmd.argv");
                    llvm::Value* cwdStr  = loadPtrField(2, "cmd.cwd");
                    llvm::Value* envArr  = loadPtrField(3, "cmd.env");
                    llvm::Value* stdinArr = loadPtrField(4, "cmd.stdin");
                    llvm::Value* stdinLen = builder->CreateLoad(i32Ty,
                        builder->CreateStructGEP(cmdStructTy, thisValue, 5,
                            "cmd.stdinLen.slot"), "cmd.stdinLen");
                    llvm::Value* flags = builder->CreateLoad(i32Ty,
                        builder->CreateStructGEP(cmdStructTy, thisValue, 6,
                            "cmd.flags.slot"), "cmd.flags");
                    llvm::Value* timeout = builder->CreateLoad(i64Ty,
                        builder->CreateStructGEP(cmdStructTy, thisValue, 7,
                            "cmd.timeout.slot"), "cmd.timeout");

                    const llvm::DataLayout& dl =
                        module->getLlvmModule()->getDataLayout();
                    const llvm::StructLayout* sSl = dl.getStructLayout(strStructTy);
                    const llvm::StructLayout* pSl = dl.getStructLayout(prStructTy);
                    // String layout: 0 vtable, 1 bytes, 2 byteLength.
                    llvm::Value* strSize    = i64c(dl.getTypeAllocSize(strStructTy));
                    llvm::Value* strOffBytes = i64c(sSl->getElementOffset(1));
                    llvm::Value* strOffLen   = i64c(sSl->getElementOffset(2));
                    // ProcessResult layout: 0 vtable, then the 8 ABI fields.
                    llvm::Value* resSize = i64c(dl.getTypeAllocSize(prStructTy));
                    llvm::Value* offLaunched = i64c(pSl->getElementOffset(1));
                    llvm::Value* offExited   = i64c(pSl->getElementOffset(2));
                    llvm::Value* offExitCode = i64c(pSl->getElementOffset(3));
                    llvm::Value* offSignaled = i64c(pSl->getElementOffset(4));
                    llvm::Value* offSignal   = i64c(pSl->getElementOffset(5));
                    llvm::Value* offTimedOut = i64c(pSl->getElementOffset(6));
                    llvm::Value* offStdout   = i64c(pSl->getElementOffset(7));
                    llvm::Value* offStderr   = i64c(pSl->getElementOffset(8));

                    llvm::Constant* prVtable =
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy));
                    if (auto* vt = prClass->getVirtualTableGlobal()) {
                        prVtable = CajetaModule::ensureGlobalInModule(
                            module->getLlvmModule(), vt);
                    }

                    llvm::Value* result = builder->CreateCall(runFn, {
                        argvArr, cwdStr, envArr, stdinArr, stdinLen, flags,
                        timeout, strSize, strOffBytes, strOffLen,
                        prVtable, resSize, offLaunched, offExited, offExitCode,
                        offSignaled, offSignal, offTimedOut, offStdout, offStderr
                    }, "cmd.run");
                    resolvedType = prClass;
                    return result;
                }
            }
        }

        // ----- cajeta.process streaming: Command.spawn() + Process.* (U2) -----
        //
        // spawn() mirrors run(): the C bridge builds the Process object (handle
        // as int64 + the three pipe fds). Process.waitFor()/waitMillis() build a
        // ProcessResult; kill()/pid()/close() operate on the handle. The pipe
        // accessors (stdin/stdout/stderr) are plain cajeta (heap FileReader/
        // FileWriter over the stored fds), so they are NOT intercepted here.
        if (thisValue && targetClass && targetClass->getQName()) {
            const std::string pc = targetClass->getQName()->toCanonical();
            const bool isCmd2  = pc == "cajeta.process.Command";
            const bool isProc2 = pc == "cajeta.process.Process";
            const bool wantSpawn = isCmd2 && methodCallName == "start"
                                   && parameters.empty();
            const bool wantWaitFor = isProc2 && methodCallName == "waitFor"
                                     && parameters.empty();
            const bool wantWaitMs = isProc2 && methodCallName == "waitMillis"
                                    && parameters.size() == 1;
            const bool wantKill = isProc2 && methodCallName == "kill"
                                  && parameters.empty();
            const bool wantPid = isProc2 && methodCallName == "pid"
                                 && parameters.empty();
            const bool wantClose = isProc2 && methodCallName == "close"
                                   && parameters.empty();
            if (wantSpawn || wantWaitFor || wantWaitMs || wantKill || wantPid
                    || wantClose) {
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                auto i64c = [&](uint64_t v) {
                    return llvm::ConstantInt::get(i64Ty, v);
                };
                auto& cmap = CajetaType::getCanonicalMap();
                auto findClass = [&](const char* canon,
                                     const char* shortName) -> CajetaClassPtr {
                    auto it = cmap.find(canon);
                    if (it == cmap.end()) it = cmap.find(shortName);
                    return it != cmap.end()
                        ? std::dynamic_pointer_cast<CajetaClass>(it->second)
                        : nullptr;
                };
                CajetaClassPtr strClass = findClass("cajeta.lang.String", "String");
                CajetaClassPtr prClass =
                    findClass("cajeta.process.ProcessResult", "ProcessResult");
                CajetaClassPtr procClass =
                    findClass("cajeta.process.Process", "Process");
                auto* strStructTy = strClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(strClass->getLlvmType())
                    : nullptr;
                auto* prStructTy = prClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(prClass->getLlvmType())
                    : nullptr;
                auto* procStructTy = procClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(procClass->getLlvmType())
                    : nullptr;
                auto* recvStructTy =
                    llvm::dyn_cast<llvm::StructType>(targetClass->getLlvmType());
                const llvm::DataLayout& dl =
                    module->getLlvmModule()->getDataLayout();

                // ProcessResult layout args (shared by waitFor/waitMillis).
                auto prLayoutArgs = [&](std::vector<llvm::Value*>& args) {
                    const llvm::StructLayout* pSl = dl.getStructLayout(prStructTy);
                    llvm::Constant* prVtable = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(ptrTy));
                    if (auto* vt = prClass->getVirtualTableGlobal())
                        prVtable = CajetaModule::ensureGlobalInModule(
                            module->getLlvmModule(), vt);
                    args.push_back(prVtable);
                    args.push_back(i64c(dl.getTypeAllocSize(prStructTy)));
                    for (unsigned i = 1; i <= 8; i++)
                        args.push_back(i64c(pSl->getElementOffset(i)));
                };

                if (wantSpawn && recvStructTy && strStructTy && procStructTy) {
                    llvm::Function* fn =
                        module->getRuntimeFunction("__cajeta_proc_spawn");
                    if (fn) {
                        auto loadPtr = [&](unsigned idx, const char* n) {
                            return builder->CreateLoad(ptrTy,
                                builder->CreateStructGEP(recvStructTy, thisValue,
                                    idx, n), n);
                        };
                        llvm::Value* argvArr = loadPtr(1, "cmd.argv");
                        llvm::Value* cwdStr  = loadPtr(2, "cmd.cwd");
                        llvm::Value* envArr  = loadPtr(3, "cmd.env");
                        llvm::Value* flags = builder->CreateLoad(i32Ty,
                            builder->CreateStructGEP(recvStructTy, thisValue, 6,
                                "cmd.flags.slot"), "cmd.flags");
                        const llvm::StructLayout* sSl =
                            dl.getStructLayout(strStructTy);
                        const llvm::StructLayout* pcSl =
                            dl.getStructLayout(procStructTy);
                        llvm::Constant* procVtable =
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy));
                        if (auto* vt = procClass->getVirtualTableGlobal())
                            procVtable = CajetaModule::ensureGlobalInModule(
                                module->getLlvmModule(), vt);
                        llvm::Value* result = builder->CreateCall(fn, {
                            argvArr, cwdStr, envArr, flags,
                            i64c(dl.getTypeAllocSize(strStructTy)),
                            i64c(sSl->getElementOffset(1)),
                            i64c(sSl->getElementOffset(2)),
                            procVtable, i64c(dl.getTypeAllocSize(procStructTy)),
                            i64c(pcSl->getElementOffset(1)),   // handleVal
                            i64c(pcSl->getElementOffset(2)),   // stdinFd
                            i64c(pcSl->getElementOffset(3)),   // stdoutFd
                            i64c(pcSl->getElementOffset(4))    // stderrFd
                        }, "cmd.spawn");
                        resolvedType = procClass;
                        return result;
                    }
                }

                // Process.* — load the int64 handle from field 1.
                if (isProc2 && recvStructTy) {
                    auto loadHandle = [&]() {
                        return builder->CreateLoad(i64Ty,
                            builder->CreateStructGEP(recvStructTy, thisValue, 1,
                                "proc.handle.slot"), "proc.handle");
                    };
                    if ((wantWaitFor || wantWaitMs) && prStructTy) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_proc_wait");
                        if (fn) {
                            llvm::Value* handle = loadHandle();
                            llvm::Value* ms = i64c((uint64_t) -1);
                            if (wantWaitMs) {
                                ms = parameters[0].expression->generateCode(module);
                                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(ms))
                                    ms = builder->CreateLoad(a->getAllocatedType(), a);
                                if (ms && ms->getType() != i64Ty
                                        && ms->getType()->isIntegerTy())
                                    ms = builder->CreateIntCast(ms, i64Ty, true);
                            }
                            std::vector<llvm::Value*> args{handle, ms};
                            prLayoutArgs(args);
                            llvm::Value* result =
                                builder->CreateCall(fn, args, "proc.wait");
                            resolvedType = prClass;
                            return result;
                        }
                    }
                    if (wantKill) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_proc_kill");
                        if (fn) {
                            builder->CreateCall(fn, {loadHandle()});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (wantPid) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_proc_pid");
                        if (fn) {
                            resolvedType = CajetaType::of("int32");
                            return builder->CreateCall(fn, {loadHandle()},
                                "proc.pid");
                        }
                    }
                    if (wantClose) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_proc_release");
                        if (fn) {
                            builder->CreateCall(fn, {loadHandle()});
                            // Zero the handle so a second close() is a no-op.
                            builder->CreateStore(i64c(0),
                                builder->CreateStructGEP(recvStructTy, thisValue,
                                    1, "proc.handle.clear"));
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                }
            }
        }

        // ----- TcpStream / TcpListener instance-method intrinsic (NET-1.3 / NET-1.4 / b1) -----
        //
        // Mirrors the File instance lowering above: the cajeta-side bodies are
        // stubs; we lower calls to the `__cajeta_net_*` runtime helpers via the
        // receiver's `fd` field (struct index 1, after the vtable — the same
        // index File.fd uses). For a byte-buffer arg we GEP past the array's
        // 8-byte count header then add the caller offset (`&buf[8 + off]`).
        if (thisValue && targetClass && targetClass->getQName()) {
            const std::string netCanonical = targetClass->getQName()->toCanonical();
            bool isTcpStream   = netCanonical == "cajeta.io.net.TcpStream";
            bool isTcpListener = netCanonical == "cajeta.io.net.TcpListener";
            // ----- UdpSocket / socket-option intrinsic (b2) -----
            bool isUdpSocket   = netCanonical == "cajeta.io.net.UdpSocket";
            if (isTcpStream || isTcpListener || isUdpSocket) {
                auto* netStructTy =
                    llvm::cast<llvm::StructType>(targetClass->getLlvmType());
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);
                // fd is the first declared field → struct index 1 (vtable@0).
                auto loadNetFd = [&]() -> llvm::Value* {
                    llvm::Value* fdSlot = builder->CreateStructGEP(
                        netStructTy, thisValue, 1, "net.fd_slot");
                    return builder->CreateLoad(i32Ty, fdSlot, "net.fd");
                };
                // &buf[8 + offset]: skip the array header, add the offset.
                auto bufPtrAtOffset = [&](size_t argIdx, llvm::Value* offV) -> llvm::Value* {
                    llvm::Value* arr = parameters[argIdx].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(arr)) {
                        arr = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    llvm::Value* dataStart = builder->CreateInBoundsGEP(
                        i8Ty, arr, llvm::ConstantInt::get(i64Ty, 8),
                        "net.buf_start");
                    return builder->CreateInBoundsGEP(
                        i8Ty, dataStart, offV, "net.buf_off");
                };
                auto loadI64Arg = [&](size_t argIdx) -> llvm::Value* {
                    llvm::Value* v = parameters[argIdx].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                        v = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    if (v && v->getType() != i64Ty && v->getType()->isIntegerTy()) {
                        v = builder->CreateIntCast(v, i64Ty, true);
                    }
                    return v;
                };

                // --- TcpStream.read(dst, offset, length) -> int64 (recv) ---
                if (isTcpStream && methodCallName == "read"
                        && parameters.size() == 3) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_recv");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* off = loadI64Arg(1);
                        llvm::Value* len = loadI64Arg(2);
                        llvm::Value* buf = bufPtrAtOffset(0, off);
                        llvm::Value* n = builder->CreateCall(fn,
                            {fd, buf, len, llvm::ConstantInt::get(i32Ty, 0)},
                            "net.recv");
                        resolvedType = CajetaType::of("int64");
                        return n;
                    }
                }
                // --- TcpStream.write(data, offset, length) -> int64 (send) ---
                if (isTcpStream && methodCallName == "write"
                        && parameters.size() == 3) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_send");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* off = loadI64Arg(1);
                        llvm::Value* len = loadI64Arg(2);
                        llvm::Value* buf = bufPtrAtOffset(0, off);
                        llvm::Value* n = builder->CreateCall(fn,
                            {fd, buf, len, llvm::ConstantInt::get(i32Ty, 0)},
                            "net.send");
                        resolvedType = CajetaType::of("int64");
                        return n;
                    }
                }
                // --- TcpStream.shutdown(how) -> void ---
                if (isTcpStream && methodCallName == "shutdown"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_shutdown");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* how = parameters[0].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(how)) {
                            how = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        if (how && how->getType() != i32Ty
                                && how->getType()->isIntegerTy()) {
                            how = builder->CreateIntCast(how, i32Ty, true);
                        }
                        builder->CreateCall(fn, {fd, how});
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                // --- TcpStream.close() / TcpListener.close() -> void ---
                if ((isTcpStream || isTcpListener) && methodCallName == "close"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_close");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        builder->CreateCall(fn, {fd});
                        // this.fd = -1 (idempotency: close(-1) is a no-op).
                        llvm::Value* fdSlot = builder->CreateStructGEP(
                            netStructTy, thisValue, 1, "net.fd_slot");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, -1), fdSlot);
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                // --- TcpListener.acceptFd() -> int32 (accept, discard peer) ---
                if (isTcpListener && methodCallName == "acceptFd"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_accept");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* nullPtr =
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy));
                        llvm::Value* connFd = builder->CreateCall(fn,
                            {fd, nullPtr, nullPtr}, "net.accept");
                        resolvedType = CajetaType::of("int32");
                        return connFd;
                    }
                }
                // --- TcpListener.boundPort() -> int32 (getsockname+unpack) ---
                if (isTcpListener && methodCallName == "boundPort"
                        && parameters.empty()) {
                    llvm::Function* nameFn = module->getRuntimeFunction(
                        "__cajeta_net_getsockname");
                    llvm::Function* unpackFn = module->getRuntimeFunction(
                        "__cajeta_net_sockaddr_unpack");
                    if (nameFn && unpackFn) {
                        llvm::Value* fd = loadNetFd();
                        // sockaddr scratch (128 = sockaddr_storage upper bound),
                        // a len in/out i32, an octets[16] out, a port i32 out.
                        llvm::Value* scratch = builder->CreateAlloca(
                            llvm::ArrayType::get(i8Ty, 128), nullptr, "net.scratch");
                        llvm::Value* lenSlot = builder->CreateAlloca(
                            i32Ty, nullptr, "net.addrlen");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, 128), lenSlot);
                        llvm::Value* octets = builder->CreateAlloca(
                            llvm::ArrayType::get(i8Ty, 16), nullptr, "net.octets");
                        llvm::Value* portSlot = builder->CreateAlloca(
                            i32Ty, nullptr, "net.port");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, 0), portSlot);
                        builder->CreateCall(nameFn, {fd, scratch, lenSlot});
                        llvm::Value* addrlen = builder->CreateLoad(
                            i32Ty, lenSlot, "net.addrlen.v");
                        builder->CreateCall(unpackFn,
                            {scratch, addrlen, octets, portSlot});
                        resolvedType = CajetaType::of("int32");
                        return builder->CreateLoad(i32Ty, portSlot, "net.boundPort");
                    }
                }

                // ----- UdpSocket / socket-option intrinsic (b2) -----
                //
                // Typed socket-option pairs (NET-1.6) on TcpStream + UdpSocket.
                // Each lowers to its dedicated get/set intrinsic via this.fd.
                // Boolean setters bind the param to a local i32 (`on ? 1 : 0`);
                // boolean getters compare the int32 intrinsic result `!= 0`.

                // Coerce a boolean/int arg to a clean i32, binding via a local
                // alloca first so a field/param-as-arg can't mistype (b1 rule).
                auto loadI32Arg = [&](size_t argIdx) -> llvm::Value* {
                    llvm::Value* v =
                        parameters[argIdx].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                        v = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    if (v && v->getType() != i32Ty && v->getType()->isIntegerTy()) {
                        v = builder->CreateIntCast(v, i32Ty, true);
                    }
                    return v;
                };
                // `on ? 1 : 0`: normalize a boolean (i1/i32) arg to 0/1.
                auto boolArgAsOnOff = [&](size_t argIdx) -> llvm::Value* {
                    llvm::Value* v = loadI32Arg(argIdx);
                    llvm::Value* nz = builder->CreateICmpNE(
                        v, llvm::ConstantInt::get(v->getType(), 0), "opt.nz");
                    return builder->CreateSelect(nz,
                        llvm::ConstantInt::get(i32Ty, 1),
                        llvm::ConstantInt::get(i32Ty, 0), "opt.onoff");
                };
                // A boolean-option setter: __cajeta_net_set_<opt>(fd, on?1:0).
                auto lowerBoolSetter = [&](const char* sym) -> bool {
                    llvm::Function* fn = module->getRuntimeFunction(sym);
                    if (!fn) return false;
                    llvm::Value* fd = loadNetFd();
                    llvm::Value* on = boolArgAsOnOff(0);
                    builder->CreateCall(fn, {fd, on});
                    resolvedType = CajetaType::of("void");
                    return true;
                };
                // A boolean-option getter: __cajeta_net_get_<opt>(fd) != 0.
                // (returns nullptr-on-miss via the `out` ref so callers can
                // distinguish "no such intrinsic" from a real i1 result.)
                auto lowerBoolGetter = [&](const char* sym,
                                           llvm::Value*& out) -> bool {
                    llvm::Function* fn = module->getRuntimeFunction(sym);
                    if (!fn) return false;
                    llvm::Value* fd = loadNetFd();
                    llvm::Value* r = builder->CreateCall(fn, {fd}, "opt.get");
                    out = builder->CreateICmpNE(
                        r, llvm::ConstantInt::get(i32Ty, 0), "opt.bool");
                    resolvedType = CajetaType::of("boolean");
                    return true;
                };
                // An int-option setter: __cajeta_net_set_<opt>(fd, value).
                auto lowerIntSetter = [&](const char* sym) -> bool {
                    llvm::Function* fn = module->getRuntimeFunction(sym);
                    if (!fn) return false;
                    llvm::Value* fd = loadNetFd();
                    llvm::Value* val = loadI32Arg(0);
                    builder->CreateCall(fn, {fd, val});
                    resolvedType = CajetaType::of("void");
                    return true;
                };
                // An int-option getter: __cajeta_net_get_<opt>(fd) -> int32.
                auto lowerIntGetter = [&](const char* sym,
                                          llvm::Value*& out) -> bool {
                    llvm::Function* fn = module->getRuntimeFunction(sym);
                    if (!fn) return false;
                    llvm::Value* fd = loadNetFd();
                    out = builder->CreateCall(fn, {fd}, "opt.iget");
                    resolvedType = CajetaType::of("int32");
                    return true;
                };

                bool isOptHolder = isTcpStream || isUdpSocket;
                if (isOptHolder) {
                    // Boolean option pairs (TcpStream NoDelay/KeepAlive;
                    // UdpSocket Broadcast). One-arg setter / zero-arg getter.
                    struct BoolOpt { const char* m; const char* setSym;
                                     const char* getSym; bool tcp; bool udp; };
                    static const BoolOpt boolOpts[] = {
                        {"NoDelay",   "__cajeta_net_set_nodelay",
                                      "__cajeta_net_get_nodelay",   true,  false},
                        {"KeepAlive", "__cajeta_net_set_keepalive",
                                      "__cajeta_net_get_keepalive", true,  false},
                        {"Broadcast", "__cajeta_net_set_broadcast",
                                      "__cajeta_net_get_broadcast", false, true},
                    };
                    for (const auto& o : boolOpts) {
                        bool applies = (isTcpStream && o.tcp)
                                    || (isUdpSocket && o.udp);
                        if (!applies) continue;
                        if (methodCallName == std::string("set") + o.m
                                && parameters.size() == 1) {
                            if (lowerBoolSetter(o.setSym)) return nullptr;
                        }
                        if (methodCallName == std::string("get") + o.m
                                && parameters.empty()) {
                            llvm::Value* out = nullptr;
                            if (lowerBoolGetter(o.getSym, out)) return out;
                        }
                    }
                    // Int option pairs: Recv/Send buffer sizes (both types).
                    struct IntOpt { const char* m; const char* setSym;
                                    const char* getSym; };
                    static const IntOpt intOpts[] = {
                        {"RecvBufferSize", "__cajeta_net_set_recvbuf",
                                           "__cajeta_net_get_recvbuf"},
                        {"SendBufferSize", "__cajeta_net_set_sendbuf",
                                           "__cajeta_net_get_sendbuf"},
                    };
                    for (const auto& o : intOpts) {
                        if (methodCallName == std::string("set") + o.m
                                && parameters.size() == 1) {
                            if (lowerIntSetter(o.setSym)) return nullptr;
                        }
                        if (methodCallName == std::string("get") + o.m
                                && parameters.empty()) {
                            llvm::Value* out = nullptr;
                            if (lowerIntGetter(o.getSym, out)) return out;
                        }
                    }
                    // TTL: the native helper takes an extra `is_v6` flag. The
                    // socket family isn't stored on the wrapper (b2 exercises
                    // V4), so pass is_v6 = 0. setTtl(int32) / getTtl().
                    if (methodCallName == "setTtl" && parameters.size() == 1) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_net_set_ttl");
                        if (fn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* ttl = loadI32Arg(0);
                            builder->CreateCall(fn,
                                {fd, llvm::ConstantInt::get(i32Ty, 0), ttl});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "getTtl" && parameters.empty()) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_net_get_ttl");
                        if (fn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* r = builder->CreateCall(fn,
                                {fd, llvm::ConstantInt::get(i32Ty, 0)},
                                "opt.ttl");
                            resolvedType = CajetaType::of("int32");
                            return r;
                        }
                    }
                }
                // Linger is TcpStream-only here (setLinger(bool,int32) /
                // getLinger()->bool reading the on-flag via the out pointers).
                if (isTcpStream && methodCallName == "setLinger"
                        && parameters.size() == 2) {
                    llvm::Function* fn =
                        module->getRuntimeFunction("__cajeta_net_set_linger");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* on = boolArgAsOnOff(0);
                        llvm::Value* secs = loadI32Arg(1);
                        builder->CreateCall(fn, {fd, on, secs});
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                if (isTcpStream && methodCallName == "getLinger"
                        && parameters.empty()) {
                    llvm::Function* fn =
                        module->getRuntimeFunction("__cajeta_net_get_linger");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* onOut = builder->CreateAlloca(
                            i32Ty, nullptr, "linger.on");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, 0), onOut);
                        llvm::Value* secsOut = builder->CreateAlloca(
                            i32Ty, nullptr, "linger.secs");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, 0), secsOut);
                        builder->CreateCall(fn, {fd, onOut, secsOut});
                        llvm::Value* onV = builder->CreateLoad(
                            i32Ty, onOut, "linger.on.v");
                        resolvedType = CajetaType::of("boolean");
                        return builder->CreateICmpNE(onV,
                            llvm::ConstantInt::get(i32Ty, 0), "linger.bool");
                    }
                }

                // ----- UdpSocket datagram I/O (b2) -----
                //
                // sendTo/recvFrom pack/unpack the SocketAddress like the b1
                // static connect/bind path; send/recv/connect/close mirror the
                // TcpStream forms. localAddress + recvFrom's `from` build a
                // SocketAddress from the unpacked sockaddr.
                if (isUdpSocket) {
                    // Pack a SocketAddress arg into a 128-byte sockaddr scratch.
                    // Returns {scratch, addrlen}; mirrors the b1 connect path.
                    auto packSockAddrArg =
                        [&](size_t argIdx, llvm::Value*& scratchOut,
                            llvm::Value*& addrlenOut) -> bool {
                        auto& cmapN = CajetaType::getCanonicalMap();
                        CajetaClassPtr saCls, ipCls;
                        auto sIt = cmapN.find("cajeta.io.net.SocketAddress");
                        if (sIt == cmapN.end()) sIt = cmapN.find("SocketAddress");
                        if (sIt != cmapN.end())
                            saCls = std::dynamic_pointer_cast<CajetaClass>(sIt->second);
                        auto iIt = cmapN.find("cajeta.io.net.IpAddress");
                        if (iIt == cmapN.end()) iIt = cmapN.find("IpAddress");
                        if (iIt != cmapN.end())
                            ipCls = std::dynamic_pointer_cast<CajetaClass>(iIt->second);
                        llvm::Function* packFn = module->getRuntimeFunction(
                            "__cajeta_net_sockaddr_pack");
                        if (!packFn || !saCls || !ipCls
                                || !llvm::isa<llvm::StructType>(saCls->getLlvmType())
                                || !llvm::isa<llvm::StructType>(ipCls->getLlvmType()))
                            return false;
                        auto* saTy = llvm::cast<llvm::StructType>(saCls->getLlvmType());
                        auto* ipTy = llvm::cast<llvm::StructType>(ipCls->getLlvmType());
                        llvm::Value* sa =
                            parameters[argIdx].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(sa)) {
                            sa = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        // sa.ip @1, sa.port @2 (vtable@0).
                        llvm::Value* ip = builder->CreateLoad(ptrTy,
                            builder->CreateStructGEP(saTy, sa, 1, "udp.ip_slot"),
                            "udp.ip");
                        llvm::Value* port = builder->CreateLoad(i32Ty,
                            builder->CreateStructGEP(saTy, sa, 2, "udp.port_slot"),
                            "udp.port");
                        // ip.family @1, ip.octets @2.
                        llvm::Value* family = builder->CreateLoad(i32Ty,
                            builder->CreateStructGEP(ipTy, ip, 1, "udp.fam_slot"),
                            "udp.family");
                        llvm::Value* octArr = builder->CreateLoad(ptrTy,
                            builder->CreateStructGEP(ipTy, ip, 2, "udp.oct_slot"),
                            "udp.octets_arr");
                        llvm::Value* octData = builder->CreateInBoundsGEP(
                            i8Ty, octArr, llvm::ConstantInt::get(i64Ty, 8),
                            "udp.octets");
                        scratchOut = builder->CreateAlloca(
                            llvm::ArrayType::get(i8Ty, 128), nullptr, "udp.sa");
                        addrlenOut = builder->CreateCall(packFn,
                            {family, octData, port, scratchOut,
                             llvm::ConstantInt::get(i32Ty, 128)}, "udp.addrlen");
                        return true;
                    };

                    // Build a heap SocketAddress from an unpacked octets[16] +
                    // host-order port + family. Allocates a fresh octet array
                    // (header'd, like `new int8[16]`) for the IpAddress so the
                    // result owns its storage. Returns the SocketAddress* (or
                    // null on a missing dependency).
                    auto buildSockAddr =
                        [&](llvm::Value* octets16, llvm::Value* portV,
                            llvm::Value* familyV) -> llvm::Value* {
                        auto& cmapN = CajetaType::getCanonicalMap();
                        CajetaClassPtr saCls, ipCls;
                        auto sIt = cmapN.find("cajeta.io.net.SocketAddress");
                        if (sIt == cmapN.end()) sIt = cmapN.find("SocketAddress");
                        if (sIt != cmapN.end())
                            saCls = std::dynamic_pointer_cast<CajetaClass>(sIt->second);
                        auto iIt = cmapN.find("cajeta.io.net.IpAddress");
                        if (iIt == cmapN.end()) iIt = cmapN.find("IpAddress");
                        if (iIt != cmapN.end())
                            ipCls = std::dynamic_pointer_cast<CajetaClass>(iIt->second);
                        llvm::Function* newArr = module->getRuntimeFunction(
                            "__cajeta_new_array_header");
                        if (!saCls || !ipCls || !newArr
                                || !llvm::isa<llvm::StructType>(saCls->getLlvmType())
                                || !llvm::isa<llvm::StructType>(ipCls->getLlvmType()))
                            return nullptr;
                        auto* saTy = llvm::cast<llvm::StructType>(saCls->getLlvmType());
                        auto* ipTy = llvm::cast<llvm::StructType>(ipCls->getLlvmType());
                        const llvm::DataLayout& dl =
                            module->getLlvmModule()->getDataLayout();
                        auto allocObj = [&](CajetaClassPtr cls,
                                            llvm::StructType* ty) -> llvm::Value* {
                            llvm::Constant* size = llvm::ConstantInt::get(
                                i64Ty, dl.getTypeAllocSize(ty));
                            llvm::Value* inst =
                                MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                            builder->CreateMemSet(inst,
                                llvm::ConstantInt::get(i8Ty, 0), size,
                                llvm::MaybeAlign(8));
                            llvm::Constant* vt =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            if (auto* g = cls->getVirtualTableGlobal()) {
                                vt = CajetaModule::ensureGlobalInModule(
                                    module->getLlvmModule(), g);
                            }
                            builder->CreateStore(vt,
                                builder->CreateStructGEP(ty, inst, 0, "sa.vt"));
                            return inst;
                        };
                        // Fresh 16-byte octet array (8-byte header + 16 bytes).
                        llvm::Value* arr = builder->CreateCall(newArr,
                            {llvm::ConstantInt::get(i64Ty, 8),
                             llvm::ConstantInt::get(i64Ty, 1),
                             llvm::ConstantInt::get(i64Ty, 16)}, "sa.octarr");
                        llvm::Value* arrData = builder->CreateInBoundsGEP(
                            i8Ty, arr, llvm::ConstantInt::get(i64Ty, 8),
                            "sa.octdata");
                        builder->CreateMemCpy(arrData, llvm::MaybeAlign(1),
                            octets16, llvm::MaybeAlign(1),
                            llvm::ConstantInt::get(i64Ty, 16));
                        // IpAddress { vtable@0, family@1, octets@2 }.
                        llvm::Value* ipInst = allocObj(ipCls, ipTy);
                        builder->CreateStore(familyV,
                            builder->CreateStructGEP(ipTy, ipInst, 1, "ip.fam"));
                        builder->CreateStore(arr,
                            builder->CreateStructGEP(ipTy, ipInst, 2, "ip.oct"));
                        // SocketAddress { vtable@0, ip@1, port@2 }.
                        llvm::Value* saInst = allocObj(saCls, saTy);
                        builder->CreateStore(ipInst,
                            builder->CreateStructGEP(saTy, saInst, 1, "sa.ip"));
                        builder->CreateStore(portV,
                            builder->CreateStructGEP(saTy, saInst, 2, "sa.port"));
                        return saInst;
                    };

                    // --- UdpSocket.sendTo(data, offset, length, dest) -> i32 ---
                    if (methodCallName == "sendTo" && parameters.size() == 4) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_sendto");
                        llvm::Value* scratch = nullptr;
                        llvm::Value* addrlen = nullptr;
                        if (fn && packSockAddrArg(3, scratch, addrlen)) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* off = loadI64Arg(1);
                            llvm::Value* len = loadI64Arg(2);
                            llvm::Value* buf = bufPtrAtOffset(0, off);
                            llvm::Value* n = builder->CreateCall(fn,
                                {fd, buf, len,
                                 llvm::ConstantInt::get(i32Ty, 0),
                                 scratch, addrlen}, "udp.sendto");
                            // sendto returns int64; the surface is int32.
                            resolvedType = CajetaType::of("int32");
                            return builder->CreateIntCast(n, i32Ty, true);
                        }
                    }
                    // --- UdpSocket.recvFrom(dst, offset, capacity) -> RecvResult ---
                    if (methodCallName == "recvFrom" && parameters.size() == 3) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_recvfrom");
                        llvm::Function* unpackFn = module->getRuntimeFunction(
                            "__cajeta_net_sockaddr_unpack");
                        auto& cmapN = CajetaType::getCanonicalMap();
                        auto rrIt = cmapN.find("cajeta.io.net.RecvResult");
                        if (rrIt == cmapN.end()) rrIt = cmapN.find("RecvResult");
                        CajetaClassPtr rrCls = rrIt != cmapN.end()
                            ? std::dynamic_pointer_cast<CajetaClass>(rrIt->second)
                            : nullptr;
                        if (fn && unpackFn && rrCls
                                && llvm::isa<llvm::StructType>(rrCls->getLlvmType())) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* off = loadI64Arg(1);
                            llvm::Value* cap = loadI64Arg(2);
                            llvm::Value* buf = bufPtrAtOffset(0, off);
                            // sockaddr out + len in/out.
                            llvm::Value* scratch = builder->CreateAlloca(
                                llvm::ArrayType::get(i8Ty, 128), nullptr,
                                "udp.rf.sa");
                            llvm::Value* lenSlot = builder->CreateAlloca(
                                i32Ty, nullptr, "udp.rf.len");
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, 128), lenSlot);
                            llvm::Value* n = builder->CreateCall(fn,
                                {fd, buf, cap,
                                 llvm::ConstantInt::get(i32Ty, 0),
                                 scratch, lenSlot}, "udp.recvfrom");
                            llvm::Value* count =
                                builder->CreateIntCast(n, i32Ty, true);
                            // Unpack the sender address.
                            llvm::Value* addrlen = builder->CreateLoad(
                                i32Ty, lenSlot, "udp.rf.len.v");
                            llvm::Value* octets = builder->CreateAlloca(
                                llvm::ArrayType::get(i8Ty, 16), nullptr,
                                "udp.rf.oct");
                            builder->CreateMemSet(octets,
                                llvm::ConstantInt::get(i8Ty, 0),
                                llvm::ConstantInt::get(i64Ty, 16),
                                llvm::MaybeAlign(1));
                            llvm::Value* portSlot = builder->CreateAlloca(
                                i32Ty, nullptr, "udp.rf.port");
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, 0), portSlot);
                            llvm::Value* fam = builder->CreateCall(unpackFn,
                                {scratch, addrlen, octets, portSlot},
                                "udp.rf.fam");
                            // unpack returns the family ordinal (0=V4) or -1;
                            // clamp a -1 to 0 so buildSockAddr stays valid.
                            llvm::Value* famOk = builder->CreateICmpSLT(fam,
                                llvm::ConstantInt::get(i32Ty, 0), "udp.rf.famneg");
                            llvm::Value* famClamped = builder->CreateSelect(
                                famOk, llvm::ConstantInt::get(i32Ty, 0), fam,
                                "udp.rf.fam.c");
                            llvm::Value* portV = builder->CreateLoad(
                                i32Ty, portSlot, "udp.rf.port.v");
                            llvm::Value* from =
                                buildSockAddr(octets, portV, famClamped);
                            // RecvResult { vtable@0, count@1, from@2 }.
                            auto* rrTy = llvm::cast<llvm::StructType>(
                                rrCls->getLlvmType());
                            const llvm::DataLayout& dl =
                                module->getLlvmModule()->getDataLayout();
                            llvm::Constant* size = llvm::ConstantInt::get(
                                i64Ty, dl.getTypeAllocSize(rrTy));
                            llvm::Value* rr =
                                MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                            builder->CreateMemSet(rr,
                                llvm::ConstantInt::get(i8Ty, 0), size,
                                llvm::MaybeAlign(8));
                            llvm::Constant* vt =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            if (auto* g = rrCls->getVirtualTableGlobal()) {
                                vt = CajetaModule::ensureGlobalInModule(
                                    module->getLlvmModule(), g);
                            }
                            builder->CreateStore(vt,
                                builder->CreateStructGEP(rrTy, rr, 0, "rr.vt"));
                            builder->CreateStore(count,
                                builder->CreateStructGEP(rrTy, rr, 1, "rr.count"));
                            if (from) {
                                builder->CreateStore(from,
                                    builder->CreateStructGEP(rrTy, rr, 2,
                                        "rr.from"));
                            }
                            resolvedType = rrCls;
                            return rr;
                        }
                    }
                    // --- UdpSocket.connect(peer) -> void ---
                    if (methodCallName == "connect" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_connect");
                        llvm::Value* scratch = nullptr;
                        llvm::Value* addrlen = nullptr;
                        if (fn && packSockAddrArg(0, scratch, addrlen)) {
                            llvm::Value* fd = loadNetFd();
                            builder->CreateCall(fn, {fd, scratch, addrlen});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    // --- UdpSocket.send(data, offset, length) -> i32 ---
                    if (methodCallName == "send" && parameters.size() == 3) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_send");
                        if (fn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* off = loadI64Arg(1);
                            llvm::Value* len = loadI64Arg(2);
                            llvm::Value* buf = bufPtrAtOffset(0, off);
                            llvm::Value* n = builder->CreateCall(fn,
                                {fd, buf, len,
                                 llvm::ConstantInt::get(i32Ty, 0)}, "udp.send");
                            resolvedType = CajetaType::of("int32");
                            return builder->CreateIntCast(n, i32Ty, true);
                        }
                    }
                    // --- UdpSocket.recv(dst, offset, capacity) -> i32 ---
                    if (methodCallName == "recv" && parameters.size() == 3) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_recv");
                        if (fn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* off = loadI64Arg(1);
                            llvm::Value* cap = loadI64Arg(2);
                            llvm::Value* buf = bufPtrAtOffset(0, off);
                            llvm::Value* n = builder->CreateCall(fn,
                                {fd, buf, cap,
                                 llvm::ConstantInt::get(i32Ty, 0)}, "udp.recv");
                            resolvedType = CajetaType::of("int32");
                            return builder->CreateIntCast(n, i32Ty, true);
                        }
                    }
                    // --- UdpSocket.localAddress() -> SocketAddress ---
                    if (methodCallName == "localAddress" && parameters.empty()) {
                        llvm::Function* nameFn = module->getRuntimeFunction(
                            "__cajeta_net_getsockname");
                        llvm::Function* unpackFn = module->getRuntimeFunction(
                            "__cajeta_net_sockaddr_unpack");
                        if (nameFn && unpackFn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* scratch = builder->CreateAlloca(
                                llvm::ArrayType::get(i8Ty, 128), nullptr,
                                "udp.la.sa");
                            llvm::Value* lenSlot = builder->CreateAlloca(
                                i32Ty, nullptr, "udp.la.len");
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, 128), lenSlot);
                            llvm::Value* octets = builder->CreateAlloca(
                                llvm::ArrayType::get(i8Ty, 16), nullptr,
                                "udp.la.oct");
                            builder->CreateMemSet(octets,
                                llvm::ConstantInt::get(i8Ty, 0),
                                llvm::ConstantInt::get(i64Ty, 16),
                                llvm::MaybeAlign(1));
                            llvm::Value* portSlot = builder->CreateAlloca(
                                i32Ty, nullptr, "udp.la.port");
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, 0), portSlot);
                            builder->CreateCall(nameFn, {fd, scratch, lenSlot});
                            llvm::Value* addrlen = builder->CreateLoad(
                                i32Ty, lenSlot, "udp.la.len.v");
                            llvm::Value* fam = builder->CreateCall(unpackFn,
                                {scratch, addrlen, octets, portSlot},
                                "udp.la.fam");
                            llvm::Value* famNeg = builder->CreateICmpSLT(fam,
                                llvm::ConstantInt::get(i32Ty, 0), "udp.la.famneg");
                            llvm::Value* famClamped = builder->CreateSelect(
                                famNeg, llvm::ConstantInt::get(i32Ty, 0), fam,
                                "udp.la.fam.c");
                            llvm::Value* portV = builder->CreateLoad(
                                i32Ty, portSlot, "udp.la.port.v");
                            llvm::Value* sa =
                                buildSockAddr(octets, portV, famClamped);
                            if (sa) {
                                auto& cmapN2 = CajetaType::getCanonicalMap();
                                auto sIt = cmapN2.find("cajeta.io.net.SocketAddress");
                                if (sIt == cmapN2.end())
                                    sIt = cmapN2.find("SocketAddress");
                                if (sIt != cmapN2.end())
                                    resolvedType =
                                        std::dynamic_pointer_cast<CajetaClass>(
                                            sIt->second);
                                return sa;
                            }
                        }
                    }
                }
            }
        }

        // Null-receiver short-circuit for class String null-safe methods.
        // Pre-Phase 2b-β these calls lowered to legacy runtime helpers
        // (__cajeta_str_len / __cajeta_str_isEmpty / __cajeta_str_equals)
        // which null-checked at the C level and returned safe defaults.
        // Class-method dispatch (vtable load through `this`) crashes on
        // a null receiver. The NullHandlingTests carry the load-bearing
        // pre-class behaviour; preserve it here by branching on the
        // receiver and selecting the safe default when null. Java NPE
        // semantics are deferred until the throws machinery covers
        // implicit null dereference (TODO in NullHandlingTests.cpp).
        bool nullSafeStringMethod = false;
        llvm::Type* nullSafeReturnTy = nullptr;
        llvm::Constant* nullSafeDefault = nullptr;
        if (thisValue && thisValue->getType()->isPointerTy() && targetClass
                && targetClass->getQName()
                && targetClass->getQName()->getTypeName() == "String"
                && targetClass->getQName()->getPackageName() == "cajeta.lang") {
            auto& ctx = *module->getLlvmContext();
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::Type* i1Ty = llvm::Type::getInt1Ty(ctx);
            if (methodCallName == "size" || methodCallName == "count") {
                // length() intentionally not in the null-safe set —
                // it's not a valid String accessor (use count() or
                // size()). Both size() and count() are int64 returns
                // and default to 0 when the receiver is null.
                nullSafeStringMethod = true;
                nullSafeReturnTy = i64Ty;
                nullSafeDefault = llvm::ConstantInt::get(i64Ty, 0);
            } else if (methodCallName == "isEmpty") {
                nullSafeStringMethod = true;
                nullSafeReturnTy = i1Ty;
                nullSafeDefault = llvm::ConstantInt::get(i1Ty, 1);
            } else if (methodCallName == "equals") {
                nullSafeStringMethod = true;
                nullSafeReturnTy = i1Ty;
                nullSafeDefault = llvm::ConstantInt::get(i1Ty, 0);
            }
        }

        llvm::BasicBlock* nullSafeNullBB = nullptr;
        llvm::BasicBlock* nullSafeCallBB = nullptr;
        llvm::BasicBlock* nullSafeJoinBB = nullptr;
        if (nullSafeStringMethod) {
            auto& ctx = *module->getLlvmContext();
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            nullSafeNullBB = llvm::BasicBlock::Create(ctx, "str.nullsafe.null", parentFn);
            nullSafeCallBB = llvm::BasicBlock::Create(ctx, "str.nullsafe.call", parentFn);
            nullSafeJoinBB = llvm::BasicBlock::Create(ctx, "str.nullsafe.join", parentFn);
            llvm::Value* isNull = builder->CreateICmpEQ(thisValue,
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(thisValue->getType())),
                "str.null");
            builder->CreateCondBr(isNull, nullSafeNullBB, nullSafeCallBB);
            builder->SetInsertPoint(nullSafeCallBB);
        }

        // # transfer at call site (auto): when the callee's formal
        // parameter is marked `#T` (isTransferred), the corresponding
        // caller-side argument's drop entry must be deactivated — the
        // callee takes ownership. Without this, a caller pattern like
        //   #Stream<T> piece = ...trySplit();
        //   shares[i] = head.cloneChainOver(piece);  // piece's drop
        //                                            // stays active
        // ends up running piece's drop at loop end AND treats the
        // shares[i] store as having received ownership — double-free
        // when the array drops at scope exit. Mirrors how
        // LocalVariableDeclaration / ArrayElementAssign already
        // deactivate the source's drop on `#`-marked or array-store
        // transfers. The check fires for any IdentifierExpression arg
        // whose Field has a drop entry (class-typed locals + owning
        // views); other arg shapes (literals, calls, dotted reads)
        // either don't have a drop entry or have it managed by their
        // own emission path. See MemoryModel.md § Borrow / transfer.
        {
            // Helper: deactivate the named local's drop entry at the
            // current insertion point. Used by both the caller-side and
            // callee-side transfer passes below.
            auto deactivateIfClassLocal = [&](size_t argIdx) {
                if (argIdx >= parameters.size()) return;
                auto argExprBase = parameters[argIdx].expression;
                auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                    argExprBase);
                if (!idExpr) return;
                auto scope = module->getScopeStack().peek();
                if (!scope) return;
                FieldPtr field = scope->getField(idExpr->getTextValue());
                if (!field) return;
                if (llvm::Value* entry = field->getDropEntry()) {
                    if (llvm::Function* mark = module->getRuntimeFunction(
                            "__cajeta_drop_mark_inactive")) {
                        builder->CreateCall(mark, {entry});
                    }
                }
            };
            // Caller-side `#x` (Phase 1 of #68). The caller's intent is
            // explicit at the source line — deactivate the source's drop
            // regardless of whether we can resolve the callee or inspect
            // its formals. Required for callees the standard non-ctor
            // resolveMethod can't find (synthesized view ctors, free
            // functions called via overloads, etc.); the caller-side
            // marker is the authoritative signal for those.
            //
            // Phase 3a of #68 (body-side check): if the `#x` source is a
            // borrowed class parameter (plain-T formal, no `#`), reject
            // — the caller is claiming to transfer a value they don't
            // own.
            for (size_t i = 0; i < parameters.size(); ++i) {
                if (!parameters[i].callerTransferred) continue;
                auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                    parameters[i].expression);
                if (idExpr) {
                    if (auto scope = module->getScopeStack().peek()) {
                        FieldPtr field = scope->getField(idExpr->getTextValue());
                        auto pf = std::dynamic_pointer_cast<ParameterField>(field);
                        if (pf) {
                            auto formal = pf->getFormalParameter();
                            if (formal && !formal->isTransferred()) {
                                auto klass = std::dynamic_pointer_cast<CajetaClass>(
                                    field->getType());
                                if (klass && !klass->isInterface()) {
                                    throw Exception(
                                        "cannot transfer borrowed parameter `"
                                            + idExpr->getTextValue() + "` via "
                                            "`#` — its formal is declared plain `T` "
                                            "(borrow), so this scope doesn't own the "
                                            "value. Fix: mark the formal `#"
                                            + idExpr->getTextValue() + "` to receive "
                                            "ownership at the outer call site, or "
                                            "restructure to not retain the borrow.",
                                        "CAJETA_ERROR_BORROW_PARAM_ESCAPES");
                                }
                            }
                        }
                    }
                }
                deactivateIfClassLocal(i);
            }
            // Callee-side `#T` formal — Phase 2 of #68: contract check.
            // When the formal is `#T` and the caller didn't acknowledge
            // transfer (no `#x` at the call site, no MoveExpression
            // wrapper, no fresh allocator), throw
            // CAJETA_ERROR_TRANSFER_REQUIRED so the caller has to either
            // surrender ownership explicitly or restructure. The check
            // fires only on a genuine double-free hazard: an
            // IdentifierExpression of a class-typed local whose Field
            // carries an active drop entry. Primitives, fresh ctors,
            // null literals, and locals without drop entries naturally
            // pass through. See docs/specification/lang/OwnershipTransfer.md.
            bool callFloatingX = true;
            for (auto& e : entries) {
                if (e.label.empty()) { callFloatingX = false; break; }
            }
            MethodPtr xferTarget = targetClass->resolveMethod(
                methodCallName, entries, /*isConstructor=*/false,
                callFloatingX);
            if (xferTarget) {
                auto formalParams = xferTarget->getParameterList();
                bool isStaticTgt = xferTarget->getModifiers().find(STATIC)
                    != xferTarget->getModifiers().end();
                bool hasThisP = !formalParams.empty()
                    && formalParams.front()->getName() == "this";
                int xferParamOffset = (isStaticTgt || !hasThisP) ? 0 : 1;
                size_t fIdx = 0;
                for (auto& fp : formalParams) {
                    if ((int) fIdx < xferParamOffset) { ++fIdx; continue; }
                    size_t argIdx = fIdx - xferParamOffset;
                    if (argIdx >= parameters.size()) break;
                    ++fIdx;
                    if (!fp->isTransferred()) continue;
                    if (parameters[argIdx].callerTransferred) continue;
                    auto argExpr = parameters[argIdx].expression;
                    if (dynamic_pointer_cast<MoveExpression>(argExpr)) continue;
                    if (dynamic_pointer_cast<NewExpression>(argExpr)) continue;
                    auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                        argExpr);
                    if (!idExpr) continue;
                    auto scope = module->getScopeStack().peek();
                    if (!scope) continue;
                    FieldPtr field = scope->getField(idExpr->getTextValue());
                    if (!field || !field->getDropEntry()) continue;
                    throw Exception(
                        "method `" + methodCallName + "` declares parameter `"
                            + fp->getName() + "` as `#T` (ownership transfer required); "
                            "write `#" + idExpr->getTextValue() + "` at the call site "
                            "to surrender ownership of the source local, or pass a "
                            "fresh `heap T(...)` / `stack T(...)` construction. "
                            "See docs/specification/lang/OwnershipTransfer.md.",
                        "CAJETA_ERROR_TRANSFER_REQUIRED");
                }
            }
        }

        // PECS write-soundness (P2-2-1). A wildcard-typed receiver
        // `Box<? extends B>` exposes parameters typed `? extends B` —
        // those slots accept reads of B (covariant) but reject writes
        // of anything other than a value sourced from the same capture
        // (Java's PECS rule). Capture identity above already lets the
        // read-back pattern through (`b.set(b.get())`); this guard
        // catches the bad-write case (`b.set(foreign)`) which today
        // silently fails to resolve. The check fires when:
        //   - targetClass is a wildcard instantiation,
        //   - it has a method by `methodCallName` with a wildcard-typed
        //     parameter at some position, and
        //   - the corresponding argument expression isn't an MCE on
        //     the same identifier-named receiver (no capture-identity
        //     evidence to make the write safe).
        if (targetClass && targetClass->isWildcardInstantiation()
                && !isSuperCall) {
            auto outerRecvId = !children.empty()
                ? dynamic_pointer_cast<IdentifierExpression>(children[0])
                : nullptr;
            for (auto& methodEntry : targetClass->getMethods()) {
                MethodPtr m = methodEntry.second;
                if (!m || m->getName() != methodCallName) continue;
                auto plist = m->getParameterList();
                bool isStatic = m->getModifiers().find(STATIC)
                    != m->getModifiers().end();
                size_t thisShift = isStatic ? 0 : 1;
                if (parameters.size() + thisShift > plist.size()) continue;
                for (size_t i = 0; i < parameters.size(); ++i) {
                    auto& formal = plist[i + thisShift];
                    if (!formal || !formal->getType()) continue;
                    // PECS: only the bounded-extends direction is the
                    // read-only producer side that breaks on writes.
                    // Unbounded `?` is type-erased dispatch (stdlib
                    // chain-walker code legitimately passes values
                    // through these slots); `? super B` is the
                    // consumer side where B-or-narrower writes ARE
                    // safe (separate slice if we ever tighten the
                    // upper bound check).
                    if (formal->getType()->wildcardKind()
                            != CajetaType::WildcardKind::Extends) {
                        continue;
                    }
                    auto argExpr = parameters[i].expression;
                    bool sameReceiverMce = false;
                    if (outerRecvId) {
                        auto argMce = dynamic_pointer_cast<MethodCallExpression>(argExpr);
                        if (argMce && !argMce->getChildren().empty()) {
                            auto argRecvId = dynamic_pointer_cast<IdentifierExpression>(
                                argMce->getChildren()[0]);
                            if (argRecvId
                                    && argRecvId->getTextValue()
                                        == outerRecvId->getTextValue()) {
                                sameReceiverMce = true;
                            }
                        }
                    }
                    if (!sameReceiverMce) {
                        throw Exception(
                            "cannot pass value to wildcard parameter '"
                            + formal->getName() + "' of "
                            + targetClass->toCanonical() + "::"
                            + methodCallName + " — the receiver's "
                            "type-parameter T is unknown beyond its bound, "
                            "so any value not sourced from this same "
                            "receiver might violate the container's "
                            "element-type contract (PECS: producer "
                            "extends, consumer super). To make the write "
                            "safe, either narrow the receiver type or "
                            "thread the value through the same receiver "
                            "(e.g. `b.set(b.get())`).",
                            "CAJETA_ERROR_PECS_WRITE_VIOLATION");
                    }
                }
                break;
            }
        }

        // Devirtualize when the static receiver type is provably its own
        // dynamic type: a `final` class has no subclasses, so the resolved
        // method is the only possible target — dispatch it directly instead of
        // through the opaque `__cajeta_vtable_lookup` runtime call. This is what
        // lets the optimizer inline leaf-class hot paths (e.g. ArrayList.add)
        // into the caller's loop, the way Rust `Vec::push` / C++ `vector::push_back`
        // inline. Interface receivers stay virtual (the static type isn't the
        // concrete impl); `invokeMethod`'s forceDirectCall path handles the rest.
        bool targetIsFinalClass = targetClass
            && !targetClass->isInterface()
            && targetClass->getModifiers().count(FINAL) > 0;
        llvm::Value* callResult = targetClass->invokeMethod(methodCallName, entries,
            /*isConstructor=*/false, thisValue, /*callerModule=*/module,
            /*forceDirectCall=*/(isSuperCall || targetIsFinalClass),
            /*explicitMethodTypeArgs=*/explicitMethodTypeArgs);

        if (nullSafeStringMethod) {
            // Close all three null-safety blocks unconditionally so the
            // function ends up with terminators on every basic block.
            // invokeMethod may return null when the method isn't
            // defined on the class (e.g. legacy `length()` calls
            // post-Phase 2b-β; class String has `size`/`count` but not
            // `length` — `length` is documented absent in String.cajeta
            // § 191). When that happens, the null-safe codegen above
            // already emitted the cond-br into call/null BBs; if we
            // leave them open, JIT verification fails with
            // "Basic Block ... does not have terminator".
            //
            // Fall back to the safe default for the null path AND for
            // the (now-unreachable) call path, so the join's phi is
            // well-formed and the surrounding code receives a value.
            // The caller (LocalVariableDeclaration / Statement::return
            // / etc.) still gets a coherent value rather than the
            // mid-emission null that triggered the verifier.
            llvm::Value* normalizedCall = callResult;
            if (callResult && callResult->getType() != nullSafeReturnTy
                    && callResult->getType()->isIntegerTy()
                    && nullSafeReturnTy->isIntegerTy()) {
                normalizedCall = builder->CreateIntCast(callResult,
                    nullSafeReturnTy, /*isSigned=*/false, "str.nullsafe.cast");
            }
            llvm::BasicBlock* callTerm = builder->GetInsertBlock();
            builder->CreateBr(nullSafeJoinBB);
            builder->SetInsertPoint(nullSafeNullBB);
            builder->CreateBr(nullSafeJoinBB);
            builder->SetInsertPoint(nullSafeJoinBB);
            llvm::PHINode* phi = builder->CreatePHI(nullSafeReturnTy, 2,
                "str.nullsafe.result");
            phi->addIncoming(
                normalizedCall ? normalizedCall : nullSafeDefault, callTerm);
            phi->addIncoming(nullSafeDefault, nullSafeNullBB);
            callResult = phi;
        }

        // Pin resolvedType to the called method's return type so a caller
        // using this MCE as a ctor / method argument can recover the
        // static type instead of CajetaType::of(value)'s opaque-pointer
        // fallback (which returns the generic `pointer` type and tripped
        // Method::buildGeneric into building the wrong lookup key — see
        // NewExpression::resolveTypes for the parallel rationale).
        // resolveMethod is cheap and idempotent: invokeMethod already
        // ran it; calling it again hits the same cached lookup. We
        // pre-check `!resolvedType` so intrinsic paths above (.stream(),
        // primitive .hash(), string intrinsics) that already pinned the
        // type aren't clobbered with a possibly-null follow-up.
        if (!resolvedType && targetClass) {
            bool callFloating = true;
            for (auto& e : entries) {
                if (e.label.empty()) { callFloating = false; break; }
            }
            // Thread the call's explicit method type-args (`parse<Box>`) into
            // resolution, exactly as the invokeMethod call above (which drives
            // codegen) already does. Without this, a templated call whose name
            // collides with a same-arity NON-template overload (e.g.
            // `Json.parse<T>(int8[],int64)` vs `Json.parse(int8[],int64)`)
            // re-resolves here to the non-template overload and pins the wrong
            // static return type (`JsonValue` instead of the instantiated `T` =
            // `Box`). resolveMethod routes a non-empty arg list through the
            // method-template resolver + instantiateMethodTemplate, so the
            // returned method's getReturnType() is the substituted `Box`.
            MethodPtr resolved = targetClass->resolveMethod(
                methodCallName, entries, /*isConstructor=*/false, callFloating,
                /*explicitMethodTypeArgs=*/explicitMethodTypeArgs);
            if (resolved && resolved->getReturnType()) {
                // Capture conversion (P2-2 item 1): when the receiver
                // is a bounded-wildcard instantiation, the method's
                // resolved return type carries the wildcard sentinel
                // in T's slot. Project to the bound so downstream
                // members (e.g., `b.get().tag()` where T is bounded
                // by Animal) resolve against the bound's surface.
                // Store the un-projected return alongside for capture-
                // identity detection at the next outer call site.
                preProjectionReturnType = resolved->getReturnType();
                resolvedType = CajetaType::captureProject(
                    preProjectionReturnType);
            }
        }

        // S9.5.5 — repackage an interface-returning call. The callee returns
        // the interface fat-pointer VALUE; downstream consumers (HeapField
        // slots, parameter pass-by-pointer) expect a body pointer, so wrap
        // the result in a fresh caller-side alloca + store.
        if (callResult && targetClass) {
            for (auto& mEntry : targetClass->getMethods()) {
                auto& m = mEntry.second;
                if (!m || m->getName() != methodCallName) continue;
                auto rt = m->getReturnType();
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
