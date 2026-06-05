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
#include "../type/CajetaView.h"
#include "../type/CajetaFunctionType.h"
#include "expression/Expression.h"
#include "expression/DotExpression.h"
#include "expression/Identifier.h"
#include "expression/MethodCallExpression.h"
#include "../method/Method.h"
#include "expression/BinaryOpExpression.h"
#include "expression/NewExpression.h"
#include "expression/AggregateInitializerExpression.h"
#include "../method/Method.h"
#include "../error/CajetaExceptions.h"
#include "../logging/CajetaLogger.h"

namespace cajeta {
    // Sizes in bytes of the runtime's drop-entry structs on the target
    // platform. Release shape is 32 bytes (obj + drop_fn + prev + active +
    // padding); debug shape is 40 (release + alloc_line + alloc_file). The
    // compiler picks which size to allocate based on CompilerFlags::sourceTags.
    // Runtime exposes __cajeta_drop_entry_size() / _size_debug() if these
    // ever need to be validated against the actual C struct sizes.
    static constexpr unsigned DROP_ENTRY_BYTES = 32;
    static constexpr unsigned DROP_ENTRY_BYTES_DEBUG = 40;

    // Pick the right entry-allocation size + push helper name + push-arg
    // shape for the active CompilerFlags. When sourceTags is on, the
    // returned helper takes 5 args (entry, obj, drop_fn, alloc_file,
    // alloc_line); when off, the original 3-arg push.
    struct DropPushChoice {
        llvm::Function* pushFn;
        unsigned entryBytes;
        bool debug;
    };
    static DropPushChoice pickDropPush(CajetaModulePtr module) {
        DropPushChoice c{};
        if (module->getFlags().sourceTags) {
            c.pushFn = module->getRuntimeFunction("__cajeta_drop_push_debug");
            c.entryBytes = DROP_ENTRY_BYTES_DEBUG;
            c.debug = true;
        } else {
            c.pushFn = module->getRuntimeFunction("__cajeta_drop_push");
            c.entryBytes = DROP_ENTRY_BYTES;
            c.debug = false;
        }
        return c;
    }

    // Emit drop-chain wiring for an owner local. Allocates a DropEntry blob on
    // the stack at function entry, pushes it onto the runtime's chain right
    // after the owner is materialized, and records the entry on both the field
    // and the enclosing method so scope-exit emits the matching pop. In debug
    // mode (CompilerFlags::sourceTags) the push variant carries the LVD's
    // source file + line for runtime diagnostics.
    static void emitDropEntryFor(CajetaModulePtr module, FieldPtr field,
                                  const std::string& dropFnName,
                                  int allocLine = 0) {
        DropPushChoice push = pickDropPush(module);
        llvm::Function* dropFn = module->getRuntimeFunction(dropFnName);
        if (!push.pushFn || !dropFn) return;
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        // Allocate the DropEntry in the function's entry block so its address
        // is stable across the function's lifetime (matters because the chain
        // threads pointers through it).
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, push.entryBytes));

        // Load the owner pointer to pass as the drop function's `obj` arg.
        llvm::Value* ownerPtr = builder->CreateLoad(ptrTy, field->getOrCreateAllocation());
        if (push.debug) {
            llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                module->getSourcePath());
            llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, allocLine);
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, dropFn, fileConst, lineConst});
        } else {
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, dropFn});
        }

        field->setDropEntry(entryPtr);
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
    }

    // Variant for class-instance locals — same drop-chain wiring, but
    // takes an already-resolved drop function (the class's synthesized
    // wrapper) directly instead of a runtime-helper name. The wrapper
    // is specific to the class, so there's no global symbol to look up
    // via getRuntimeFunction.
    static void emitDropEntryForFn(CajetaModulePtr module, FieldPtr field,
                                    llvm::Function* dropFn,
                                    int allocLine = 0) {
        DropPushChoice push = pickDropPush(module);
        if (!push.pushFn || !dropFn) return;
        // Cross-module: when the class whose drop fn we're pushing
        // lives in a different llvm::Module (a stdlib class
        // referenced from user code), substitute a module-local
        // extern decl so the merge step resolves the Constant.
        dropFn = CajetaModule::ensureFunctionInModule(
            module->getLlvmModule(), dropFn);
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, push.entryBytes));

        llvm::Value* ownerPtr = builder->CreateLoad(ptrTy, field->getOrCreateAllocation());
        if (push.debug) {
            llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                module->getSourcePath());
            llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, allocLine);
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, dropFn, fileConst, lineConst});
        } else {
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, dropFn});
        }

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
        // Only true primitives (int32, float64, bool, etc.) — and @ValueType POD
        // values, which are by-value/Copy like primitives — get an inline-value
        // alloca holding the struct itself. See plans/value-type-overloading-plan.md.
        bool isArray = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
        bool wantsInlineSlot = type->hasValueSemantics() && !isArray;
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
            // isn't always populated. See cajeta-docs/stdlib/Lambdas.md.
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

            // Polymorphic-MI upcast adjustment. After HeapField stored
            // the RHS pointer into the slot, check whether the static
            // RHS type is a non-self class subclass of the LHS class
            // type. If so — and the LHS isn't on the RHS's first-parent
            // chain (offset != 0) — reload, shift to the LHS's sub-
            // object start, and store back. This makes
            // `B b = c` actually point at C's B sub-object rather than
            // at C itself, so dispatch via b lands on the secondary
            // vtable + B-shaped field offsets.
            if (initializer && type) {
                auto dstClass = dynamic_pointer_cast<CajetaClass>(type);
                if (dstClass && !dstClass->isInterface()) {
                    auto varInit = dynamic_pointer_cast<VariableInitializer>(
                        initializer);
                    CajetaTypePtr srcType;
                    if (varInit && !varInit->getChildren().empty()) {
                        if (auto expr = dynamic_pointer_cast<Expression>(
                                varInit->getChildren()[0])) {
                            if (!expr->getResolvedType()) {
                                expr->resolveTypes(module);
                            }
                            srcType = expr->getResolvedType();
                        }
                    }
                    auto srcClass = dynamic_pointer_cast<CajetaClass>(srcType);
                    if (srcClass && srcClass.get() != dstClass.get()
                            && !srcClass->isInterface()) {
                        uint64_t off = srcClass->getSubObjectByteOffset(
                            dstClass.get());
                        if (off != 0) {
                            auto* builder = module->getBuilder();
                            auto& ctx = *module->getLlvmContext();
                            llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
                            llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
                            llvm::Value* slot = field->getOrCreateAllocation();
                            llvm::Value* raw = builder->CreateLoad(
                                ptrTy, slot, "upcast_raw");
                            llvm::Value* adjusted = builder->CreateInBoundsGEP(
                                i8Ty, raw,
                                llvm::ConstantInt::get(
                                    llvm::Type::getInt64Ty(ctx), off),
                                "upcast_subobj");
                            builder->CreateStore(adjusted, slot);
                        }
                    }
                }
            }

            // P3 — definite-assignment tracking. A local declared without
            // an initializer enters the scope's NYA set; reading it before
            // an assignment is a compile error. Applies to both class-
            // typed locals (a null reference is meaningful, but reading
            // it would be a runtime null-deref) and primitive locals (the
            // alloca contents are undefined until written). Skipped for
            // view/struct locals that get their body pointer wired via
            // the S6.1 implicit alloca path below — those are stack-
            // resident with zero-init bodies, "assigned" by virtue of
            // pointing at a real body. Also skipped for interface-typed
            // locals (the interface-local handling block above stores a
            // body ptr in the slot regardless of initializer).
            if (!initializer) {
                bool implicitZeroInit =
                    dynamic_pointer_cast<CajetaView>(type) != nullptr;
                bool isInterface = false;
                if (auto kc = dynamic_pointer_cast<CajetaClass>(type)) {
                    isInterface = kc->isInterface();
                }
                if (!implicitZeroInit && !isInterface) {
                    module->getScopeStack().peek()->markNotYetAssigned(
                        declarator->getIdentifier());
                }
            }

            // `class Foo f;` without an initializer lands as an NYA-marked
            // null class ref via the path above. Instantiation is always
            // explicit: `heap Foo()` / `stack Foo()` / `Foo { x: 1, y: 2 }`.

            // Interface local handling. An interface local's
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

                            // Class RHS wrapped in MoveExpression
                            // (`Greeter g = #h;` or `Greeter g = #heap Hello();`)
                            // sets OWNED_CLASS; bare class RHS sets
                            // BORROWED_CLASS. MoveExpression's own
                            // generateCode has already marked the source
                            // moved + deactivated its drop entry, so
                            // ownership transfers cleanly to the interface
                            // value's drop chain (drop dispatches on kind).
                            bool rhsIsMove = dynamic_pointer_cast<MoveExpression>(rhsExpr) != nullptr;
                            int64_t kindValue;
                            if (rhsIsMove) {
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
                    // View-aliasing: when the initializer is a view
                    // construction call like `Header(bytes)`, record which
                    // field the view aliases. ReturnStatement consults this
                    // to reject returning a view of a same-scope local
                    // buffer. The shape we recognize:
                    //   - MethodCallExpression with NO receiver
                    //   - MCE's name matches a registered CajetaView
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
            // P2a — `stack MyClass(args)` produces an instance owned by
            // the current frame (alloca-backed body); the class's heap
            // destructor would free a stack pointer if we registered the
            // usual drop entry. Detect by inspecting the init AST for a
            // NewExpression with stackAlloc=true and skip the heap-drop
            // registration. KNOWN LIMITATION: owned class-ref FIELDS of
            // a stack-allocated owner leak in v1 because we skip the
            // drop entirely; Phase 2b adds a stack-drop variant that
            // walks owned fields without freeing the body.
            bool initIsStackAlloc = false;
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    auto rhsExpr = dynamic_pointer_cast<Expression>(children[0]);
                    if (auto newExpr = dynamic_pointer_cast<NewExpression>(children[0])) {
                        if (newExpr->getStackAlloc()) {
                            initIsStackAlloc = true;
                        }
                    }
                    // P2b — `stack MyClass { f: v }` on a plain class
                    // returns an alloca'd body pointer; the class-drop
                    // would free a stack pointer. Detect by inspecting
                    // for stack-flagged AggregateInitializerExpression.
                    if (auto aggExpr = dynamic_pointer_cast<AggregateInitializerExpression>(children[0])) {
                        if (aggExpr->getStackAlloc()) {
                            initIsStackAlloc = true;
                        }
                    }
                    if (auto mc = dynamic_pointer_cast<MethodCallExpression>(children[0])) {
                        if (mc->getMethodCallName() == "__cajeta_inject") {
                            initIsBorrow = true;
                        } else {
                            // Resolve the called method at decl time so a
                            // non-`#` (borrow) return doesn't register a
                            // drop entry on the receiving local — that
                            // would double-free a borrowed reference at
                            // scope exit (e.g. `Stream<T> n =
                            // cur.unwrap();` where unwrap returns a plain
                            // Stream<T> field of cur — the field's owner
                            // already tracks the drop).
                            //
                            // Walk mc's first child (the receiver, when
                            // present) to find the target class. A bare
                            // method call (no receiver) is on `this`;
                            // get it from the current class on the
                            // structure stack.
                            auto mcKids = mc->getChildren();
                            CajetaClassPtr targetCls;
                            if (!mcKids.empty()) {
                                auto recvExpr = dynamic_pointer_cast<Expression>(mcKids[0]);
                                if (recvExpr) {
                                    if (!recvExpr->getResolvedType()) {
                                        recvExpr->resolveTypes(module);
                                    }
                                    targetCls = dynamic_pointer_cast<CajetaClass>(
                                        recvExpr->getResolvedType());
                                }
                            } else if (!module->getStructureStack().empty()) {
                                targetCls = dynamic_pointer_cast<CajetaClass>(
                                    module->getStructureStack().back());
                            }
                            if (targetCls) {
                                vector<ParameterEntry> mcEntries;
                                bool floatingAll = true;
                                for (auto& p : mc->getParameters()) {
                                    if (!p.expression->getResolvedType()) {
                                        p.expression->resolveTypes(module);
                                    }
                                    CajetaTypePtr pType =
                                        p.expression->getResolvedType();
                                    if (p.label.empty()) floatingAll = false;
                                    mcEntries.push_back(
                                        ParameterEntry(pType, p.label, nullptr));
                                }
                                string mcName = mc->getMethodCallName();
                                MethodPtr resolved = targetCls->resolveMethod(
                                    mcName, mcEntries,
                                    /*isConstructor=*/false, floatingAll);
                                if (resolved && !resolved->isReturnsOwnership()) {
                                    // Non-# return — the local is a borrow
                                    // of whatever the callee returned.
                                    initIsBorrow = true;
                                }
                            }
                        }
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
                            bool rhsIsStruct = dynamic_pointer_cast<CajetaView>(rhsExpr->getResolvedType()) != nullptr;
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
                            if (rhsClass) {
                                initIsBorrow = true;
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
                emitDropEntryFor(module, field, "__cajeta_free_array", getSourceLine());
            }

            // Gap 4 — record a live read-borrow on the scope so a later
            // write through the borrowed path (or any prefix of it)
            // rejects with CAJETA_ERROR_USE_AFTER_MOVE before clobbering
            // the borrowed slot. Triggered for the borrow shapes
            // detected above: field/array reads of class refs
            // (`String alias = p.name`) and local-to-local aliases of
            // class refs (`String alias = other`). Struct-typed locals
            // initIsBorrow shape ALSO goes through here, which the
            // existing alias machinery already treats as a move — the
            // borrow record is harmless there since the source is
            // simultaneously marked moved.
            // Gap 4 (live read-borrows) and Gap 5 (owned String drops).
            // A String / class / array local with a path-shaped
            // initializer (`String alias = p.name;`, `Foo b = a;`)
            // aliases its source — record a live borrow so a later
            // write to the source's path rejects.
            // A String local initialized from a known-allocating
            // string helper (`String r = "hello".concat(" world");`
            // or `String r = a + b;`) owns the malloc'd buffer —
            // register a free drop entry so the buffer doesn't leak
            // at scope exit. Two paths handled here:
            //   1. MethodCallExpression on a String receiver with an
            //      allocating method name (concat/substring/
            //      toUpperCase/toLowerCase/trim/replace).
            //   2. BinaryOpExpression with operator + producing a
            //      pointer-typed result (lowered via
            //      __cajeta_str_concat in BinaryOpExpression::ADD).
            //
            // This drop registration applies only to the LEGACY
            // primitive-alias String path (i8* C-strings, with malloc'd
            // buffers that need free). cajeta.lang.String (the class
            // form) follows the never-drop rule per
            // cajeta-docs/stdlib/lang/String.md § Memory model — its
            // method implementations don't register drops at all, and
            // its substring is a view, not an allocation.
            //
            // The two categories are mutually exclusive: only one of
            // borrow / owned applies to a given initializer.
            // Borrow recording fires for any local that aliases its
            // source rather than copying. Two conditions admit:
            //   (a) llvmType is a pointer (legacy primitive-String /
            //       array shapes — heap header is a malloc'd buffer).
            //   (b) type is a CajetaClass (class instance — the local
            //       holds a heap pointer in a struct-typed slot).
            // Interfaces and views aren't borrow candidates (their
            // own slots are fat-pointers / inline structs).
            bool typeIsClass = std::dynamic_pointer_cast<CajetaClass>(type) != nullptr
                && type
                && !(type->getQName()
                    && std::dynamic_pointer_cast<CajetaClass>(type)->isInterface());
            bool ptrLike = type && type->getLlvmType()
                && type->getLlvmType()->isPointerTy();
            if (field && initializer && type && (ptrLike || typeIsClass)) {
                if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                    auto& children = varInit->getChildren();
                    if (!children.empty()) {
                        auto child = children[0];

                        // Owned-string detection. Only applies to the
                        // LEGACY primitive String shape (i8* malloc'd
                        // buffer) — the class String follows the never-
                        // drop rule (cajeta-docs/stdlib/lang/String.md
                        // § Memory model) and registering a drop here
                        // would double-free. Gate on ptrLike: class-
                        // typed locals (typeIsClass branch) skip this
                        // block entirely and fall through to borrow
                        // recording only.
                        bool producesOwnedString = false;
                        if (ptrLike && !typeIsClass) {
                        if (auto mc = dynamic_pointer_cast<MethodCallExpression>(child)) {
                            const std::string& name = mc->getMethodCallName();
                            if (name == "concat" || name == "substring"
                                    || name == "toUpperCase" || name == "toLowerCase"
                                    || name == "trim" || name == "replace") {
                                auto& mcChildren = mc->getChildren();
                                if (!mcChildren.empty()) {
                                    auto recv = dynamic_pointer_cast<Expression>(mcChildren[0]);
                                    if (recv) {
                                        if (!recv->getResolvedType()) {
                                            recv->resolveTypes(module);
                                        }
                                        auto rt = recv->getResolvedType();
                                        if (rt && rt->getQName()
                                                && rt->getQName()->getTypeName() == "String") {
                                            producesOwnedString = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (!producesOwnedString) {
                            if (auto bo = dynamic_pointer_cast<BinaryOpExpression>(child)) {
                                if (bo->getBinaryOp() == BINARY_OP_ADD) {
                                    if (!bo->getResolvedType()) {
                                        bo->resolveTypes(module);
                                    }
                                    auto rt = bo->getResolvedType();
                                    if (rt && rt->getQName()
                                            && rt->getQName()->getTypeName() == "String"
                                            && rt->getLlvmType()
                                            && rt->getLlvmType()->isPointerTy()) {
                                        producesOwnedString = true;
                                    }
                                }
                            }
                        }
                        } // end: if (ptrLike && !typeIsClass)

                        if (producesOwnedString) {
                            emitDropEntryFor(module, field, "__cajeta_free", getSourceLine());
                        } else {
                            // Borrow recording (Gap 4).
                            string borrowedPath;
                            if (auto dot = dynamic_pointer_cast<DotExpression>(child)) {
                                borrowedPath = DotExpression::buildPath(dot);
                            } else if (auto id = dynamic_pointer_cast<IdentifierExpression>(child)) {
                                borrowedPath = id->getTextValue();
                            }
                            if (!borrowedPath.empty()) {
                                if (auto sc = module->getScopeStack().peek()) {
                                    sc->recordLiveBorrow(field->getName(), borrowedPath);
                                }
                            }
                        }
                    }
                }
            }

            // Owning view (`View(#buf)`): the view took ownership of the
            // buffer from its source (the MoveExpression deactivated the
            // source's drop entry inside MoveExpression::generateCode).
            // Register a fresh drop entry against the view's data pointer
            // — __cajeta_view_drop_owned reconstructs the array header by
            // subtracting the 8-byte header offset and frees it.
            if (field->isOwningView()) {
                emitDropEntryFor(module, field, "__cajeta_view_drop_owned", getSourceLine());
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
                emitDropEntryFor(module, field, "__cajeta_closure_drop", getSourceLine());
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
            bool isStructType = dynamic_pointer_cast<CajetaView>(type) != nullptr;
            // P3 — when there's no initializer, the slot's contents are
            // undefined at declaration time. Registering a drop here
            // would capture the slot's value at PUSH time (garbage), so
            // a later `c = heap Counter()` assignment would leave the
            // drop entry pointing at the original garbage and free that
            // address at scope exit (crash). Skip the drop registration
            // in the no-initializer case; this leaks the heap instance
            // assigned via the later `c = heap X()` but doesn't crash.
            // Follow-on work: defer drop registration until first
            // assignment (or shift the drop entry to load the slot at
            // fire time instead of push time).
            // cajeta.lang.String is process-lifetime — view-mode
            // literals live in static storage, owned-mode allocations
            // (concat results, substring copies, etc.) are intentionally
            // never reclaimed per the never-drop spec
            // (cajeta-docs/stdlib/lang/String.md § Memory model). Skip
            // the drop wiring entirely; vtable.drop_fn stays NULL.
            // (Reclaiming would require a boundary-transfer mechanism
            // for cajeta heap that escapes to C++ via the JIT lookup
            // — MD5/SipHash/XXHash3 test frameworks return `s.bytes` to
            // the test and read from it after the cajeta function
            // returns. Without that mechanism, enabling drops here
            // turns those tests into use-after-frees.)
            bool isCajetaString = klass && klass->getQName()
                && klass->getQName()->getTypeName() == "String"
                && klass->getQName()->getPackageName() == "cajeta.lang";
            // @ValueType locals are Copy PODs living inline in their slot —
            // never heap-backed, no owned fields, no destructor. They must NOT
            // enter the drop chain: a drop-push here would load the slot's
            // first word (the vtable pointer) and register the value-type body
            // for a spurious stack/virtual drop at scope exit. Skip entirely.
            if (klass && !isArray && !isStructType && !klass->isInterface()
                    && !initIsBorrow && initializer && !isCajetaString
                    && !klass->isValueType()) {
                // P7.1/P7.2 — stack-allocated class locals (init via
                // `stack ClassName(...)` or `stack ClassName { ... }`)
                // get the stack-drop variant: walks owned class-ref
                // fields + recurses into embedded structs, but does
                // NOT free the body (function epilogue handles that).
                // Stack allocation fixes the dynamic type (alloca size
                // is sized for the declared class), so static dispatch
                // here is correct.
                //
                // Heap-class locals go through __cajeta_class_virtual_drop
                // — the dispatcher loads the instance's vtable and calls
                // the drop_fn slot, so a base-typed local holding a
                // derived instance (`Animal a = heap Dog()`) fires
                // ~Dog() rather than ~Animal() (MemoryModel.md Gap 1).
                // The static per-class drop wrappers are still emitted
                // and reachable: they live in vtable.drop_fn and the
                // dispatcher routes through them.
                if (initIsStackAlloc) {
                    if (llvm::Function* stackDropFn =
                            klass->getOrCreateStackDropFunction()) {
                        emitDropEntryForFn(module, field, stackDropFn, getSourceLine());
                    }
                } else if (klass->hasVtablePointerAtSlotZero()) {
                    // Patch this class's vtable drop_fn slot with the
                    // synthesized heap-drop wrapper, then register the
                    // runtime dispatcher (Gap 1 — virtual dispatch on
                    // drop). The dispatcher loads vtable.drop_fn from
                    // the instance at scope exit, so a base-typed local
                    // holding a derived instance fires the derived's
                    // destructor.
                    klass->patchVirtualTableDropFn();
                    emitDropEntryFor(module, field,
                        "__cajeta_class_virtual_drop", getSourceLine());
                } else {
                    // Custom-layout classes (CajetaTask<T> — no vtable
                    // pointer at slot 0). The virtual dispatcher can't
                    // safely read from instance[0], so fall back to
                    // static dispatch. These types are monomorphic by
                    // construction (no user subclassing), so the static
                    // per-class drop fn is both correct and sufficient.
                    if (llvm::Function* dropFn =
                            klass->getOrCreateDropFunction()) {
                        emitDropEntryForFn(module, field, dropFn, getSourceLine());
                    }
                }
            }

            // Stack-resident class instances flow through the regular
            // stack-drop path via klass->getOrCreateStackDropFunction()
            // wired earlier in this method.

            // Interface local drop entry. Pushes a drop entry
            // pointing at __cajeta_iface_drop, the kind-tag dispatcher.
            // The helper reads the fat pointer's kind word and either
            // invokes the per-(class, iface) vtable's drop slot
            // (OWNED_CLASS) or no-ops (BORROWED_*). Cheap to push for
            // every interface local because the BORROWED branches are
            // bare return statements.
            if (klass && klass->isInterface()) {
                emitDropEntryFor(module, field, "__cajeta_iface_drop", getSourceLine());
            }
        }

        return nullptr;
    }

} // code