//
// Created by James Klappenbach on 11/4/22.
//

#include "LocalVariableDeclaration.h"
#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../field/HeapField.h"
#include "../field/StackField.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaStruct.h"
#include "../type/CajetaView.h"
#include "../type/CajetaFunctionType.h"
#include "expression/Expression.h"
#include "expression/DotExpression.h"
#include "expression/Identifier.h"
#include "expression/MethodCallExpression.h"
#include "../method/Method.h"
#include "../error/CajetaExceptions.h"
#include "../logging/CajetaLogger.h"

namespace cajeta {
    // Size in bytes of the runtime's cajeta_drop_entry struct on the target
    // platform. The runtime exposes __cajeta_drop_entry_size() if we ever need
    // to validate this; for x86-64 and aarch64 the struct fits in 32 bytes
    // (8 byte obj ptr + 8 byte drop fn ptr + 8 byte prev ptr + 1 byte active +
    // padding).
    static constexpr unsigned DROP_ENTRY_BYTES = 32;

    // Emit drop-chain wiring for an owner local. Allocates a DropEntry blob on
    // the stack at function entry, pushes it onto the runtime's chain right
    // after the owner is materialized, and records the entry on both the field
    // and the enclosing method so scope-exit emits the matching pop.
    static void emitDropEntryFor(CajetaModulePtr module, FieldPtr field,
                                  const std::string& dropFnName) {
        llvm::Function* push = module->getRuntimeFunction("__cajeta_drop_push");
        llvm::Function* dropFn = module->getRuntimeFunction(dropFnName);
        if (!push || !dropFn) return;
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        // Allocate the DropEntry in the function's entry block so its address
        // is stable across the function's lifetime (matters because the chain
        // threads pointers through it).
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, DROP_ENTRY_BYTES));

        // Load the owner pointer to pass as the drop function's `obj` arg.
        llvm::Value* ownerPtr = builder->CreateLoad(ptrTy, field->getOrCreateAllocation());
        builder->CreateCall(push, {entryPtr, ownerPtr, dropFn});

        field->setDropEntry(entryPtr);
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
    }

    // Variant for class-instance locals — same drop-chain wiring, but
    // takes an already-resolved drop function (the class's synthesized
    // wrapper) directly instead of a runtime-helper name. The wrapper
    // is specific to the class, so there's no global symbol to look up
    // via getRuntimeFunction.
    static void emitDropEntryForFn(CajetaModulePtr module, FieldPtr field,
                                    llvm::Function* dropFn) {
        llvm::Function* push = module->getRuntimeFunction("__cajeta_drop_push");
        if (!push || !dropFn) return;
        // Cross-module: when the class whose drop fn we're pushing
        // lives in a different llvm::Module (a stdlib class
        // referenced from user code), substitute a module-local
        // extern decl so the merge step resolves the Constant.
        dropFn = CajetaModule::ensureFunctionInModule(
            module->getLlvmModule(), dropFn);
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, DROP_ENTRY_BYTES));

        llvm::Value* ownerPtr = builder->CreateLoad(ptrTy, field->getOrCreateAllocation());
        builder->CreateCall(push, {entryPtr, ownerPtr, dropFn});

        field->setDropEntry(entryPtr);
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
    }

    /**
     * If we have a primitive variable, we can store in on the stack and will immediately create an currentRegister.
     * Otherwise, we will create an currentRegister for a structure reference.  If the variable receives a new operator,
     * we'll just let the malloc call create the register
     *
     * @param module
     * @return
     */
    llvm::Value* LocalVariableDeclaration::generateCode(CajetaModulePtr module) {

        // Arrays and class instances live on the heap; their local slot is a pointer.
        // Only true primitives (int32, float64, bool, etc.) get an inline-value alloca.
        bool isArray = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
        bool wantsInlineSlot = (type->getTypeFlags() & PRIMITIVE_FLAG) && !isArray;
        for (auto& declarator: variableDeclarators) {
            InitializerPtr initializer = declarator->getInitializer();
            // Array-literal initializer (`int32[] xs = {1, 2, 3}`): the
            // literal has no type of its own, so push the element type
            // down here before codegen so the literal knows how big the
            // slots are and how to coerce its values.
            if (isArray) {
                if (auto arrInit = dynamic_pointer_cast<ArrayInitializer>(initializer)) {
                    if (auto arrType = dynamic_pointer_cast<CajetaArray>(type)) {
                        arrInit->setElementType(arrType->getElementType());
                    }
                }
            }
            // Function-typed initializer with a lambda RHS: push the LHS's
            // function type down to the lambda so it can use the declared
            // return type (and, eventually, expected param types) rather
            // than trying to infer them from a body whose own resolvedType
            // isn't always populated. See cajeta-docs/Lambdas.md.
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    if (auto lambda = dynamic_pointer_cast<LambdaExpression>(children[0])) {
                        lambda->setExpectedType(type);
                    }
                }
            }
            FieldPtr field;
            if (wantsInlineSlot) {
                field = make_shared<StackField>(module, declarator->getIdentifier(), type,
                    declarator->isReference(), modifiers, annotations, initializer);
            } else {
                field = make_shared<HeapField>(module, declarator->getIdentifier(), type,
                    declarator->isReference(), modifiers, annotations, initializer);
            }
            module->getScopeStack().peek()->putField(field);
            field->getOrCreateAllocation();

            // S6.1 — `struct Foo f;` lays a fresh stack alloca of the struct
            // body and zero-initializes it; the HeapField's pointer slot
            // points at that body. Treating the local as a pointer to an
            // aggregate keeps DotExpression / parameter-passing identical
            // to the view path (also aggregate-by-pointer). View locals
            // skip this — their pointer comes from the view-ctor result.
            // Aggregate-initializer support (`Foo { x: 1, y: 2 }`) lands
            // in S6.2 and will replace the zero-init with per-field stores.
            if (dynamic_pointer_cast<CajetaStruct>(type)
                    && !dynamic_pointer_cast<CajetaView>(type)
                    && !initializer) {
                auto* builder = module->getBuilder();
                llvm::Type* bodyTy = type->getLlvmType();
                llvm::Value* bodyAlloca = builder->CreateAlloca(bodyTy);
                builder->CreateStore(llvm::Constant::getNullValue(bodyTy),
                    bodyAlloca);
                builder->CreateStore(bodyAlloca, field->getOrCreateAllocation());
            }

            // S9.5.4 — interface local handling. An interface local's
            // HeapField slot holds a `ptr` pointing at a 24-byte
            // fat-pointer body. Three init shapes:
            //   - No initializer: allocate empty body, zero-init, store
            //     body ptr in slot.
            //   - Initializer RHS is an interface value (already a body
            //     ptr after loadIfLValue's S9.5.4 branch): HeapField
            //     stored the body ptr in the slot; the local aliases the
            //     source body (borrow). No additional work here.
            //   - Initializer RHS is a non-interface class value (e.g.
            //     `Greeter g = new Hello()`): HeapField stored the
            //     class instance pointer in the slot, but the local
            //     needs a 24-byte fat-pointer body. Build the body
            //     (data = class ptr, vtable = per-(class, iface) global,
            //     kind = BORROWED_CLASS for v1 — `#`-marked owned land
            //     in S10.2) and overwrite the slot to point at it.
            if (auto ifaceKlass = dynamic_pointer_cast<CajetaClass>(type)) {
                if (ifaceKlass->isInterface()) {
                    auto* builder = module->getBuilder();
                    auto& lctx = *module->getLlvmContext();
                    llvm::Type* bodyTy = type->getLlvmType();
                    llvm::Type* ptrTy = llvm::PointerType::get(lctx, 0);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(lctx);

                    if (!initializer) {
                        llvm::Value* bodyAlloca = builder->CreateAlloca(bodyTy);
                        builder->CreateStore(
                            llvm::Constant::getNullValue(bodyTy), bodyAlloca);
                        builder->CreateStore(bodyAlloca,
                            field->getOrCreateAllocation());
                    } else {
                        auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer);
                        ExpressionPtr rhsExpr;
                        if (varInit && !varInit->getChildren().empty()) {
                            rhsExpr = dynamic_pointer_cast<Expression>(
                                varInit->getChildren()[0]);
                            if (rhsExpr && !rhsExpr->getResolvedType()) {
                                rhsExpr->resolveTypes(module);
                            }
                        }
                        CajetaTypePtr rhsType = rhsExpr
                            ? rhsExpr->getResolvedType() : nullptr;
                        auto rhsClass = dynamic_pointer_cast<CajetaClass>(rhsType);
                        bool rhsIsInterface = rhsClass && rhsClass->isInterface();

                        if (rhsClass && !rhsIsInterface) {
                            // Re-load whatever HeapField stored in the slot.
                            // For a class instance RHS that's the heap class
                            // pointer; for a struct RHS that's the struct
                            // body pointer (S6.2 / S6.7 / aliasing flows
                            // all hand off ptr-to-body as the struct value).
                            // Either way the value goes into the fat
                            // pointer's data slot.
                            llvm::Value* sourcePtr = builder->CreateLoad(
                                ptrTy, field->getOrCreateAllocation());

                            llvm::Value* bodyAlloca = builder->CreateAlloca(bodyTy);
                            llvm::Value* dataSlot = builder->CreateStructGEP(
                                bodyTy, bodyAlloca, 0, "iface_data");
                            llvm::Value* vtSlot = builder->CreateStructGEP(
                                bodyTy, bodyAlloca, 1, "iface_vtable");
                            llvm::Value* kindSlot = builder->CreateStructGEP(
                                bodyTy, bodyAlloca, 2, "iface_kind");
                            builder->CreateStore(sourcePtr, dataSlot);

                            std::string ifaceCanonical =
                                ifaceKlass->getQName()->toCanonical();
                            llvm::Constant* vtableRef = nullptr;
                            if (auto gv = rhsClass->getInterfaceVTable(ifaceCanonical)) {
                                vtableRef = CajetaModule::ensureGlobalInModule(
                                    module->getLlvmModule(), gv);
                            }
                            if (!vtableRef) {
                                vtableRef = llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            }
                            builder->CreateStore(vtableRef, vtSlot);

                            // S10.1 — struct RHS sets BORROWED_STRUCT.
                            // S10.2 — class RHS wrapped in MoveExpression
                            // (`Greeter g = #h;` or `Greeter g = #new Hello();`)
                            // sets OWNED_CLASS; bare class RHS sets
                            // BORROWED_CLASS in v1. MoveExpression's own
                            // generateCode has already marked the source
                            // moved + deactivated its drop entry, so
                            // ownership transfers cleanly to the interface
                            // value's drop chain (S10.4 dispatches on kind).
                            // Note: `#` on a struct RHS is a no-op for the
                            // kind tag — structs are stack-resident and can't
                            // be owned by an interface value; the BORROWED
                            // tag stands.
                            bool rhsIsStruct = dynamic_pointer_cast<CajetaStruct>(rhsType) != nullptr;
                            bool rhsIsMove = dynamic_pointer_cast<MoveExpression>(rhsExpr) != nullptr;
                            int64_t kindValue;
                            if (rhsIsStruct) {
                                kindValue = IFACE_KIND_BORROWED_STRUCT;
                                // S10.3 — mark the interface local as
                                // borrowing a struct that lives in the
                                // current function frame. ReturnStatement
                                // consults this to reject returns that
                                // would dangle the data ptr.
                                field->setInterfaceBorrowsStructLocal(true);
                            } else if (rhsIsMove) {
                                kindValue = IFACE_KIND_OWNED_CLASS;
                            } else {
                                kindValue = IFACE_KIND_BORROWED_CLASS;
                            }
                            builder->CreateStore(
                                llvm::ConstantInt::get(i64Ty,
                                    (uint64_t) kindValue),
                                kindSlot);
                            builder->CreateStore(bodyAlloca,
                                field->getOrCreateAllocation());
                        }
                        // RHS is interface: loadIfLValue returned a body
                        // ptr, HeapField stored it. Local now aliases the
                        // source body (BORROWED). No fix-up needed.
                    }
                }
            }

            // L3-2 escape-check wiring: a function-typed local initialized
            // from a lambda or bound method reference inherits the RHS's
            // borrow-capture state. After the initializer has run
            // (putField → getOrCreateAllocation triggered the RHS's
            // generateCode and its capture analysis), copy the flag onto
            // the field so a later `return fnLocal` can surface the
            // dangling-borrow error before LLVM verify.
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    if (auto lambda = dynamic_pointer_cast<LambdaExpression>(children[0])) {
                        if (lambda->getHasBorrowCaptures()) {
                            field->setHasBorrowCaptures(true);
                        }
                    }
                    if (auto methodRef = dynamic_pointer_cast<MethodReferenceExpression>(children[0])) {
                        if (methodRef->getHasBorrowCaptures()) {
                            field->setHasBorrowCaptures(true);
                        }
                    }
                    // Ownership-transfer (option a) for spawn → local. The
                    // spawn pushed its own drop entry inside its
                    // generateCode so that bare-statement `spawn foo();`
                    // (no local to attach to) still gets freed at scope
                    // exit. When the result IS bound to a named local,
                    // that local's class-instance drop entry below
                    // becomes the canonical owner — mark the spawn's
                    // transient entry inactive so it doesn't double-fire.
                    // Mirrors how `#`-move-out marks the source inactive.
                    // See AsyncStatus.md § Plan: Task<T> as user-typeable
                    // template / Ownership-transfer model.
                    if (auto spawn = dynamic_pointer_cast<SpawnExpression>(children[0])) {
                        if (llvm::Value* spawnEntry = spawn->getDropEntry()) {
                            if (llvm::Function* markInactive = module->getRuntimeFunction(
                                    "__cajeta_drop_mark_inactive")) {
                                module->getBuilder()->CreateCall(
                                    markInactive, {spawnEntry});
                            }
                        }
                    }
                    // Struct view-aliasing: when the initializer is a
                    // struct-view construction call like `Header(bytes)`,
                    // record which field the view aliases. ReturnStatement
                    // consults this to reject returning a view of a same-
                    // scope local buffer. The shape we recognize:
                    //   - MethodCallExpression with NO receiver (children[0]
                    //     of the MCE itself is absent; we already filtered
                    //     to the call here via children[0] of the
                    //     VariableInitializer)
                    //   - MCE's name matches a registered CajetaStruct
                    //   - Exactly one parameter, an IdentifierExpression
                    //     resolving to a field in the current scope
                    // Anything else (multi-arg ctor, dynamically-built
                    // buffer arg, etc.) is left untracked for v1 — the
                    // common footgun is `Header h = Header(localBytes);
                    // return h;` and that's what we catch.
                    if (auto mce = dynamic_pointer_cast<MethodCallExpression>(children[0])) {
                        bool isViewCtor = mce->getChildren().empty()
                            && mce->getParameters().size() == 1
                            && dynamic_pointer_cast<CajetaView>(
                                CajetaType::of(mce->getMethodCallName())) != nullptr;
                        if (isViewCtor) {
                            auto& mceParams = mce->getParameters();
                            auto argExpr = mceParams[0].expression;
                            // Owning vs borrow form (Views.md § Construction).
                            // `View(#buf)` transfers buffer ownership to the
                            // view; `View(buf)` borrows. The MoveExpression
                            // wrapper at the argument site is the discriminator.
                            // For owning form we skip setViewSource so the
                            // borrow-checker treats the view as an owner
                            // (returnable, transferable, no escape error);
                            // a deferred drop-entry registration further down
                            // handles scope-exit cleanup.
                            bool isOwning = dynamic_pointer_cast<MoveExpression>(argExpr) != nullptr;
                            field->setIsOwningView(isOwning);
                            if (!isOwning) {
                                if (auto idArg = dynamic_pointer_cast<IdentifierExpression>(argExpr)) {
                                    auto scope = module->getScopeStack().peek();
                                    FieldPtr src = scope
                                        ? scope->getField(idArg->getTextValue())
                                        : nullptr;
                                    if (src) {
                                        field->setViewSource(src);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Borrow detection runs FIRST so each drop-entry site
            // below can honor it. Without this ordering, the array
            // drop fires unconditionally and `T[] alias = paramArr`
            // double-frees at scope exit.
            //
            // When the RHS does NOT transfer ownership of a fresh
            // allocation to the local, the local must NOT register a
            // drop entry. Recognized borrow sources:
            //
            //   1. __cajeta_inject() — returns the DI singleton
            //      (A9). A drop here would double-free on the
            //      second @Inject site or free state still in use
            //      elsewhere.
            //   2. DotExpression reading a class-typed field — the
            //      local now points at an instance owned by
            //      whatever object holds the field. The owner is
            //      responsible for the drop; the borrowing local
            //      must not duplicate it.
            //
            // Both shapes are observable directly from the
            // initializer expression's AST type. NewExpression
            // (fresh malloc) and most other initializers retain
            // their ownership-transfer semantics.
            bool initIsBorrow = false;
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    auto rhsExpr = dynamic_pointer_cast<Expression>(children[0]);
                    if (auto mc = dynamic_pointer_cast<MethodCallExpression>(children[0])) {
                        if (mc->getMethodCallName() == "__cajeta_inject") {
                            initIsBorrow = true;
                        }
                        // GAP: MethodCallExpression initializers should
                        // distinguish move (called method returns
                        // ownership via `#T foo()`) from borrow (called
                        // method returns a plain T). That requires the
                        // called method to be resolved here, but
                        // resolution happens during generateCode rather
                        // than at LocalVariableDeclaration time. Until
                        // a resolve-at-decl-time path exists, every
                        // method-call init defaults to MOVE — a
                        // false-positive double-free is louder than a
                        // false-negative leak, so leaning that way is
                        // safer. Add a resolve pre-pass and consult
                        // method->isReturnsOwnership() when wired.
                    } else if (dynamic_pointer_cast<DotExpression>(children[0])
                            || dynamic_pointer_cast<ArrayIndexExpression>(children[0])) {
                        // The RHS is a field read or an array element
                        // read. If the value type is class-like (and
                        // not a struct), the local is a pointer to
                        // an object owned by the receiver (for a
                        // field) or by the array (for an element).
                        // Treat the local as a borrow — registering
                        // a drop entry would double-free at scope
                        // exit since the owner still tracks the
                        // instance.
                        if (rhsExpr) {
                            if (!rhsExpr->getResolvedType()) {
                                rhsExpr->resolveTypes(module);
                            }
                            auto rhsClass = dynamic_pointer_cast<CajetaClass>(rhsExpr->getResolvedType());
                            bool rhsIsStruct = dynamic_pointer_cast<CajetaAggregate>(rhsExpr->getResolvedType()) != nullptr;
                            if (rhsClass && !rhsIsStruct) {
                                initIsBorrow = true;
                            }
                        }
                    } else if (auto rhsId = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                        // The RHS is another named binding (local var
                        // or method parameter). Two cases:
                        //
                        //   - Class-like, non-struct: the new local is
                        //     a second pointer to the same heap object;
                        //     the original owner is responsible for the
                        //     drop. Mark as borrow so the drop chain
                        //     doesn't double-free.
                        //
                        //   - Struct: the new local aliases the source
                        //     struct's body alloca. With both locals
                        //     registering independent drop entries (the
                        //     pre-fix behavior) the struct drop fn ran
                        //     twice on the same body — double-freeing
                        //     any owned class-ref fields. Fix: treat
                        //     `Foo b = a;` as a MOVE — suppress b's
                        //     drop registration AND mark `a` as moved.
                        //     The source local's drop entry stays
                        //     active (since `a` keeps the body
                        //     pointer); reads of `a` after this point
                        //     trip CAJETA_ERROR_USE_AFTER_MOVE.
                        //
                        // This complements the field-read and array-
                        // element cases above; together they cover the
                        // three common "alias an existing heap object"
                        // shapes.
                        if (rhsExpr) {
                            if (!rhsExpr->getResolvedType()) {
                                rhsExpr->resolveTypes(module);
                            }
                            auto rhsClass = dynamic_pointer_cast<CajetaClass>(rhsExpr->getResolvedType());
                            bool rhsIsStruct = dynamic_pointer_cast<CajetaStruct>(rhsExpr->getResolvedType()) != nullptr;
                            if (rhsClass && !rhsIsStruct) {
                                initIsBorrow = true;
                            } else if (rhsIsStruct) {
                                initIsBorrow = true;
                                if (auto scope = module->getScopeStack().peek()) {
                                    scope->markMoved(rhsId->getTextValue());
                                }
                            }
                        }
                    }
                }
            }

            // Array-typed locals own the heap header; register
            // __cajeta_free_array unless this local is a borrow.
            // The borrow case (e.g. `T[] alias = paramArr` or
            // `T[] xs = obj.field`) already has an owner upstream;
            // duplicating the drop here double-frees at scope exit.
            if (isArray && !initIsBorrow) {
                emitDropEntryFor(module, field, "__cajeta_free_array");
            }

            // Owning view (`View(#buf)`): the view took ownership of the
            // buffer from its source (the MoveExpression deactivated the
            // source's drop entry inside MoveExpression::generateCode).
            // Register a fresh drop entry against the view's data pointer
            // — __cajeta_view_drop_owned reconstructs the array header by
            // subtracting the 8-byte header offset and frees it.
            if (field->isOwningView()) {
                emitDropEntryFor(module, field, "__cajeta_view_drop_owned");
            }

            // L3-3: function-typed locals own the closure record they
            // point at (for capturing closures) and need a drop entry
            // that fires __cajeta_closure_drop at scope exit. Non-
            // capturing closures store a stack-allocated record with
            // drop_fn=null, so the runtime helper no-ops on them; the
            // entry shape is therefore safe for every function-typed
            // local regardless of what it holds. ReturnStatement
            // deactivates the entry when the local is returned so
            // ownership transfers to the caller without a double-free.
            if (dynamic_pointer_cast<CajetaFunctionType>(type)) {
                emitDropEntryFor(module, field, "__cajeta_closure_drop");
            }

            // User-defined-drop wiring for class-instance locals.
            // Arrays and structs are handled separately above; struct
            // values live inline (no heap), and arrays have their own
            // __cajeta_free_array path. For everything else that's a
            // class — including the constructor-ref result — register
            // the class's synthesized drop wrapper so the instance is
            // reclaimed at scope exit (running any user-declared
            // `drop()` method first). ReturnStatement deactivates this
            // entry when the local is returned, transferring ownership
            // to the caller.
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            bool isStructType = dynamic_pointer_cast<CajetaAggregate>(type) != nullptr;
            if (klass && !isArray && !isStructType && !klass->isInterface()
                    && !initIsBorrow) {
                if (llvm::Function* dropFn = klass->getOrCreateDropFunction()) {
                    emitDropEntryForFn(module, field, dropFn);
                }
            }

            // S6.4 — struct local drop entry. Push a drop entry pointing at
            // the struct's synthesized drop fn so any owned class-ref fields
            // get reclaimed at scope exit. Views are excluded (they have
            // their own owning-view drop wired earlier). The struct's drop
            // fn doesn't free the body (stack-resident); it just walks
            // class-ref fields and calls each referent's drop. Aggregate
            // init's per-binding ownership-transfer (S6.4) is what keeps
            // this from double-freeing the source locals whose class
            // instances were moved into the struct. `initIsBorrow` gates
            // the alias case (`Foo b = a;`) — the source local keeps the
            // drop entry, b doesn't register a second one.
            if (auto structType = dynamic_pointer_cast<CajetaStruct>(type)) {
                if (!initIsBorrow) {
                    if (llvm::Function* structDropFn =
                            structType->getOrCreateDropFunction()) {
                        emitDropEntryForFn(module, field, structDropFn);
                    }
                }
            }
        }

        return nullptr;
    }

} // code