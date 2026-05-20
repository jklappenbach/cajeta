//
// Created by James Klappenbach on 4/19/23.
//

#include "MethodCallExpression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaView.h"
#include "cajeta/type/CajetaFunctionType.h"
#include "cajeta/method/Method.h"
#include "cajeta/error/Exception.h"
#include "cajeta/util/MemoryManager.h"
#include "Expression.h"
#include "DotExpression.h"
#include "Identifier.h"
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

    // methodCall: `identifier ('<' typeList '>')? '(' parameterList? ')'`
    //           | `THIS '(' parameterList? ')'`
    //           | `SUPER '(' parameterList? ')'`
    //
    // The optional `<typeList>` between name and '(' is Form C explicit
    // call-site type args for method-templated callees (see
    // cajeta-docs/stdlib/MethodLevelTemplate.md). Inference is the
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
                parameters.push_back(entry);
            }
        }
        // Form C call-site type args. Each typeType resolves through
        // CajetaType::fromContext under the active module's substitution
        // stack so a `<T>` referenced from inside a templated class body
        // still resolves to the bound T.
        if (auto* tl = ctx->typeList()) {
            auto activeMod = CajetaModule::getActiveModule();
            for (auto* tt : tl->typeType()) {
                auto t = CajetaType::fromContext(tt, activeMod);
                if (t) explicitMethodTypeArgs.push_back(t);
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
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
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
        auto argExpr = std::dynamic_pointer_cast<Expression>(argNode);
        CajetaTypePtr argTy = argExpr ? argExpr->getResolvedType() : nullptr;
        if (!argTy && argExpr) {
            argExpr->resolveTypes(module);
            argTy = argExpr->getResolvedType();
        }
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
            std::vector<ParameterEntry> entries;
            for (auto& p : parameters) {
                if (p.expression && !p.expression->getResolvedType()) {
                    p.expression->resolveTypes(module);
                }
                llvm::Value* v = p.expression ? p.expression->generateCode(module) : nullptr;
                CajetaTypePtr t = p.expression ? p.expression->getResolvedType() : nullptr;
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
        // See cajeta-docs/stdlib/Lambdas.md.
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

                return dataPtr;
            }
        }

        // ----- Bare class-construction syntax rejected (cajeta-docs/stdlib/UnifiedClasses.md P1b) -----
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
                // cajeta-docs/stdlib/Thread.md § Synchronization primitives.
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
                } else if (dynamic_pointer_cast<DotExpression>(exprChild)
                        && llvm::isa<llvm::GetElementPtrInst>(receiver)
                        && receiverType
                        && dynamic_pointer_cast<CajetaClass>(receiverType)
                        && !dynamic_pointer_cast<CajetaView>(receiverType)) {
                    // Chained field access `a.b.method()` where `b` is a
                    // class-ref OR array field. DotExpression returned the
                    // field's slot pointer (a GEP); the slot stores a `ptr`
                    // to the referent instance / array header (per
                    // CajetaClass::fieldLayoutType rule that lays class-ref
                    // and array fields as `ptr`). Load through to
                    // materialize the instance/header pointer used as the
                    // dispatch receiver. Without this the vtable load at
                    // instance[0] would read the SLOT's first word (which
                    // is the instance ptr itself, not the vtable), and
                    // __cajeta_vtable_lookup would walk garbage; for arrays
                    // the same shape — `.count()` / `.stream()` would read
                    // the slot's first 8 bytes (the header pointer) as if
                    // it were the count word. View and interface fields
                    // stay inline, so the slot IS the language-level value
                    // — skip the load there.
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
                    if (it != cmap.end()
                            && dynamic_pointer_cast<CajetaClass>(it->second)) {
                        receiverType = it->second;
                    }
                }
            }
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
            if (recvClass) {
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
                    vector<llvm::Value*> args;
                    args.push_back(captures);
                    llvm::FunctionType* sig = fnType->getLlvmFunctionType();
                    for (size_t i = 0; i < parameters.size(); ++i) {
                        llvm::Value* v = parameters[i].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                            v = builder->CreateLoad(a->getAllocatedType(), a);
                        }
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
                    // Pin resolvedType so a caller using this MCE as an
                    // argument can recover the lambda's declared return
                    // type (mirrors the post-invokeMethod resolvedType
                    // pinning further below).
                    if (fnType->getReturnType()) {
                        resolvedType = fnType->getReturnType();
                    }
                    return builder->CreateCall(sig, callee, args);
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
        bool anyLambda = false;
        for (auto& param : parameters) {
            if (std::dynamic_pointer_cast<LambdaExpression>(param.expression)
                    && !param.expression->getResolvedType()) {
                anyLambda = true;
                break;
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
                        // Method-templates carry only `this` in
                        // parameterList until instantiation; defer the
                        // arity check to after we instantiate (below).
                        // When explicit type-args are present and the
                        // template's type-param count matches, accept
                        // the template as a candidate and let the
                        // instantiation step verify arity.
                        bool isMethodTpl = m->isMethodTemplate();
                        if (isMethodTpl && !explicitMethodTypeArgs.empty()
                                && explicitMethodTypeArgs.size()
                                    == m->getMethodTypeParameters().size()) {
                            candidate = m;
                            ++matches;
                            continue;
                        }
                        if (isMethodTpl) continue;
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
                        if (!lambda->getResolvedType() && p->getType()) {
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
            if (dynamic_pointer_cast<DotExpression>(param.expression)) {
                value = loadIfLValue(module, value, param.expression);
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

        // MultiClassing Phase 3 v3 (cajeta-docs/stdlib/MultiClassing.md
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
        // spec in cajeta-docs/stdlib/io/file/{FileReader,FileWriter,
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
            if (methodCallName == "size" || methodCallName == "length"
                    || methodCallName == "count") {
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

        llvm::Value* callResult = targetClass->invokeMethod(methodCallName, entries,
            /*isConstructor=*/false, thisValue, /*callerModule=*/module,
            /*forceDirectCall=*/isSuperCall,
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
            MethodPtr resolved = targetClass->resolveMethod(
                methodCallName, entries, /*isConstructor=*/false, callFloating);
            if (resolved && resolved->getReturnType()) {
                resolvedType = resolved->getReturnType();
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
