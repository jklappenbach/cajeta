//
// Created by James Klappenbach on 4/19/23.
//

#include "CreatorRest.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/util/MemoryManager.h"
#include "cajeta/asn/expression/Expression.h"
#include "cajeta/method/Method.h"

#include <functional>

namespace cajeta {
    shared_ptr<CreatorRest> CreatorRest::fromContext(CajetaParser::CreatorContext* ctx, antlr4::Token* token) {
        if (ctx->classCreatorRest()) {
            return make_shared<ClassCreatorRest>(ctx->classCreatorRest(), token);
        } else {
            return make_shared<ArrayCreatorRest>(ctx->arrayCreatorRest(), token);
        }
    }

    // Emits `malloc(sizeof(struct))` (or, when stackAlloc is set, an entry-
    // block alloca) then dispatches to the matching constructor with the
    // user-supplied arguments. Returns the instance pointer either way.
    llvm::Value* ClassCreatorRest::generateCode(CajetaModulePtr module) {
        if (!targetType) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        llvm::LLVMContext& llvmCtx = *module->getLlvmContext();
        llvm::Type* structTy = targetType->getLlvmType();
        const llvm::DataLayout& dataLayout = module->getLlvmModule()->getDataLayout();
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(llvmCtx),
            dataLayout.getTypeAllocSize(structTy));

        llvm::Value* instance;
        if (nrvoTarget) {
            // NRVO: build directly into the caller's sret slot — no separate
            // alloca/malloc and no copy. The memset + vtable init + ctor below
            // all run against the caller-owned slot.
            instance = nrvoTarget;
        } else if (stackAlloc) {
            // P2a — entry-block alloca for `stack MyClass(args)`. Hoist the
            // alloca to the function entry so it lives for the whole frame
            // (LLVM convention; allocas in arbitrary blocks are legal but
            // confuse mem2reg + can grow the stack frame across loop
            // iterations). Lifetime is the enclosing scope; the borrow
            // checker rejects escape (return / heap-field-store) per the
            // S10.3-generalized check.
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
                parentFn->getEntryBlock().begin());
            instance = entryBuilder.CreateAlloca(structTy);
        } else {
            instance = MemoryManager::createMallocInstruction(
                module, allocSize, builder->GetInsertBlock());
        }

        // S7.2 — zero-init the instance block. malloc returns uninitialized
        // bytes; alloca's contents are also unspecified. The vtable slot
        // below gets overwritten, and most fields get written by the
        // user's constructor body. But fields the ctor doesn't explicitly
        // initialize stay at whatever the allocator gave us — garbage.
        // That's load-bearing for class-drop recursion into embedded
        // struct fields (S7.2): the recursive struct drop reads class-ref
        // slots inside the embedded struct, and a non-null garbage pointer
        // slips past the Tracer-drop null guard and crashes on free().
        // Zero-init matches JVM / .NET defaults too; class-ref fields read
        // as null until the ctor writes them.
        builder->CreateMemSet(instance,
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvmCtx), 0),
            allocSize, llvm::MaybeAlign(8));

        // Initialize the vtable pointer at instance slot 0. Required for
        // dynamic dispatch — `dog.speak()` reads slot 0 to find Dog's vtable
        // before binary-searching for `speak`'s hash. Without this write the
        // instance's vtable pointer is zero from the memset above, and the
        // first virtual call segfaults.
        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            if (llvm::GlobalVariable* vtable = klass->getVirtualTableGlobal()) {
                // Cross-module: when targetType lives in a different
                // llvm::Module than where the `new` is being emitted
                // (multi-source compile), substitute a module-local
                // extern decl so the merge can reconcile it.
                llvm::Constant* vtableRef = CajetaModule::ensureGlobalInModule(
                    module->getLlvmModule(), vtable);
                llvm::Value* vtablePtrSlot = builder->CreateStructGEP(
                    structTy, instance, /*idx=*/0, "vtable_slot");
                builder->CreateStore(vtableRef, vtablePtrSlot);
            }

            // Polymorphic-MI: secondary vtable per non-first-parent
            // sub-object. The primary vtable above lives at slot 0 and
            // is read when dispatching through this class's own type;
            // dispatching through a non-first-parent-typed binding
            // reads the vptr at the start of that parent's sub-object,
            // which is one of the slots enumerated by
            // klass->getNonFirstSubObjects(). Each such slot gets a
            // dedicated secondary vtable (built lazily and cached on
            // klass), structurally compatible with the parent's
            // standalone vtable but carrying this class's overrides
            // (via offset thunks where the impl lives elsewhere).
            for (const auto& sub : klass->getNonFirstSubObjects()) {
                llvm::GlobalVariable* secVT =
                    klass->getOrCreateSecondaryVTable(sub.ancestor);
                if (!secVT) continue;
                llvm::Constant* secRef = CajetaModule::ensureGlobalInModule(
                    module->getLlvmModule(), secVT);
                llvm::Value* secSlot = builder->CreateStructGEP(
                    structTy, instance, (unsigned) sub.slot,
                    std::string("sec_vtable_slot_")
                        + sub.ancestor->getQName()->getTypeName());
                builder->CreateStore(secRef, secSlot);
            }
            // Gap 1 (virtual dispatch on drop). The instance carries this
            // class's vtable regardless of the declared type of the
            // binding (`Animal a = heap Dog()` stores Dog's vtable). At
            // scope exit __cajeta_class_virtual_drop loads vtable.drop_fn
            // — patch this class's vtable slot now so dispatch routes to
            // ~Dog() rather than ~Animal(). Stack allocations skip this
            // path (no malloc, vtable still set for method dispatch, but
            // the drop chain registers a static stack-drop fn — see
            // LocalVariableDeclaration::generateCode).
            //
            // Custom-layout classes (CajetaTask<T>'s { fn, arg, done,
            // ... } body has no vtable pointer at slot 0) skip the
            // patch — their drop registration site falls back to
            // static dispatch.
            if (!stackAlloc && klass->hasVtablePointerAtSlotZero()) {
                klass->patchVirtualTableDropFn();
            }
        }

        // Lambda-as-ctor-arg expectedType propagation. Mirror the
        // MethodCallExpression pattern so a bare-identifier lambda
        // passed to `heap T(...)` / `stack T(...)` / `new T(...)`
        // borrows its param + return types from the matching ctor's
        // formal. Without this, `heap Holder(seed, (acc, x) -> ...)`
        // fails type inference (no LHS signal, no propagator),
        // forcing every caller to spell typed params explicitly.
        //
        // Match the ctor by arity. Constructors are non-static, so the
        // formal param list includes `this` as element 0 — subtract 1.
        // Template-instantiation skip from MCE doesn't fire here (ctors
        // aren't method-templated under the current grammar — the doc
        // explicitly excludes constructors from method-level templates,
        // see cajeta-docs/stdlib/MethodLevelTemplate.md § Constructors
        // and operators excluded).
        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            bool anyLambda = false;
            for (auto& param : parameters) {
                if (std::dynamic_pointer_cast<LambdaExpression>(param.expression)
                        && !param.expression->getResolvedType()) {
                    anyLambda = true;
                    break;
                }
            }
            if (anyLambda) {
                std::string ctorName = targetType->getQName()->getTypeName();
                if (klass->getTemplateOrigin()) {
                    ctorName = klass->getTemplateOrigin()->getQName()->getTypeName();
                }
                MethodPtr candidate;
                int matches = 0;
                for (auto& mEntry : klass->getMethods()) {
                    auto& m = mEntry.second;
                    if (!m->isConstructor()) continue;
                    if (m->getName() != ctorName) continue;
                    int declared = (int) m->getParameterList().size() - 1;
                    if (declared != (int) parameters.size()) continue;
                    candidate = m;
                    ++matches;
                }
                if (candidate && matches == 1) {
                    auto paramList = candidate->getParameterList();
                    // paramList[0] is `this`; user-visible args start at 1.
                    for (size_t i = 0; i < parameters.size(); ++i) {
                        size_t formalIdx = i + 1;
                        if (formalIdx >= paramList.size()) break;
                        if (auto lambda = std::dynamic_pointer_cast<LambdaExpression>(
                                parameters[i].expression)) {
                            if (!lambda->getResolvedType()
                                    && paramList[formalIdx]->getType()) {
                                lambda->setExpectedType(
                                    paramList[formalIdx]->getType());
                            }
                        }
                    }
                }
            }
        }

        // Resolve parameters and call the constructor. Prefer the expression's
        // resolvedType when available — `CajetaType::of(llvm::Value*)` can't
        // recover class-instance types (the LLVM value is just a `ptr` and
        // doesn't carry the user-class identity). Without this fallback,
        // passing a class instance as a constructor arg would null-deref
        // when `Method::buildCanonical` walks parameter types.
        vector<ParameterEntry> entries;
        for (auto& param : parameters) {
            if (!param.expression->getResolvedType()) {
                param.expression->resolveTypes(module);
            }
            llvm::Value* value = param.expression->generateCode(module);
            // L-value-to-r-value coercion. Argument expressions can be
            // local allocas (IdentifierExpression), field GEPs
            // (DotExpression — covers `this.handle` etc.), or array
            // slots (ArrayIndexExpression) — all need a load before
            // the value flows into the constructor. Without this,
            // ctor params like `pointer handle` receive the slot
            // address instead of the value, which silently misroutes
            // every pthread-handle-style argument.
            auto astExpr = dynamic_pointer_cast<Expression>(param.expression);
            value = loadIfLValue(module, value, astExpr);
            CajetaTypePtr paramType = param.expression->getResolvedType();
            if (!paramType) paramType = CajetaType::of(value);
            entries.push_back(ParameterEntry(paramType, param.label, value));
        }
        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            // Constructor name is the class's simple type name. For a
            // template instantiation (e.g. "Container<int32>") the
            // source-parsed ctor was named after the unparameterized
            // template ("Container") — fall through to the template
            // origin's name so we find the real ctor instead of looking
            // up "Container<int32>" and falling back to the empty
            // auto-default. Pairs with the Method ctor's same fallback
            // (Method.cpp's constructor-detection logic).
            string ctorName = targetType->getQName()->getTypeName();
            if (klass->getTemplateOrigin()) {
                ctorName = klass->getTemplateOrigin()->getQName()->getTypeName();
            }
            klass->invokeMethod(ctorName, entries, /*isConstructor=*/true, instance,
                                /*callerModule=*/module);
        }
        return instance;
    }

    // Java-style array allocation: one heap call per dimension level. For `new T[a][b]`
    // we allocate the outer header of length `a` whose element-type is a pointer to
    // an inner array, then loop and allocate an inner header of length `b` for each
    // outer slot. For `new T[a][]` we only allocate the outer; inner slots stay null
    // (the runtime helper zero-fills via calloc). Returns the outermost header pointer.
    llvm::Value* ArrayCreatorRest::generateCode(CajetaModulePtr module) {
        if (!targetType || totalBracketPairs <= 0) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();

        // Build the type chain: typeChain[0] = T (innermost element),
        // typeChain[k] = T[][...] wrapped k times. typeChain[N] is the outermost type.
        vector<CajetaTypePtr> typeChain;
        typeChain.push_back(targetType);
        for (int i = 0; i < totalBracketPairs; i++) {
            CajetaTypePtr wrapped = make_shared<CajetaArray>(module, typeChain.back());
            module->getStructures()[wrapped->toCanonical()] =
                static_pointer_cast<CajetaClass>(wrapped);
            typeChain.push_back(wrapped);
        }

        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_new_array_header");
        if (!allocFn) {
            return nullptr;
        }
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        // Recursive emitter: level 0 = outermost. Allocates that level's header and,
        // when an inner size was specified, loops over the data slots populating them
        // with recursive sub-allocations.
        std::function<llvm::Value*(int)> emit = [&](int level) -> llvm::Value* {
            if (level >= (int) children.size()) {
                return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0));
            }
            auto arr = dynamic_pointer_cast<CajetaArray>(typeChain[totalBracketPairs - level]);
            if (!arr) {
                return nullptr;
            }

            // Resolve this level's user-supplied size, loaded if it came from an alloca.
            llvm::Value* count = children[level]->generateCode(module);
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(count)) {
                count = builder->CreateLoad(a->getAllocatedType(), a);
            }
            if (count->getType() != i64Ty) {
                count = builder->CreateIntCast(count, i64Ty, /*isSigned=*/true);
            }

            llvm::Type* headerTy = arr->getLlvmType();
            llvm::Type* elemTy = arr->getElementLlvmType(&ctx);
            llvm::Value* headerSize = llvm::ConstantInt::get(i64Ty,
                dl.getTypeAllocSize(headerTy));
            llvm::Value* elemSize = llvm::ConstantInt::get(i64Ty,
                dl.getTypeAllocSize(elemTy));
            llvm::Value* hdrPtr = builder->CreateCall(allocFn,
                {headerSize, elemSize, count});

            // If there's a deeper level to populate, loop over `count` slots and assign.
            if (level + 1 < (int) children.size()) {
                // Counter alloca at function entry to keep the loop clean of repeated allocas.
                llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
                    parentFn->getEntryBlock().begin());
                llvm::Value* counterAlloca = entryBuilder.CreateAlloca(i64Ty);
                builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), counterAlloca);

                llvm::BasicBlock* loopHead = llvm::BasicBlock::Create(ctx, "arr_init_head", parentFn);
                llvm::BasicBlock* loopBody = llvm::BasicBlock::Create(ctx, "arr_init_body", parentFn);
                llvm::BasicBlock* loopExit = llvm::BasicBlock::Create(ctx, "arr_init_exit", parentFn);

                builder->CreateBr(loopHead);

                builder->SetInsertPoint(loopHead);
                llvm::Value* idx = builder->CreateLoad(i64Ty, counterAlloca);
                llvm::Value* cmp = builder->CreateICmpSLT(idx, count);
                builder->CreateCondBr(cmp, loopBody, loopExit);

                builder->SetInsertPoint(loopBody);
                llvm::Value* inner = emit(level + 1);
                // Slot = &hdrPtr->data[idx]. GEP indices walk: pointer -> struct -> data array -> element.
                vector<llvm::Value*> gepIndices = {
                    llvm::ConstantInt::get(i64Ty, 0),
                    llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
                    idx,
                };
                llvm::Value* slot = builder->CreateGEP(headerTy, hdrPtr, gepIndices);
                builder->CreateStore(inner, slot);
                llvm::Value* nextIdx = builder->CreateAdd(idx,
                    llvm::ConstantInt::get(i64Ty, 1));
                builder->CreateStore(nextIdx, counterAlloca);
                builder->CreateBr(loopHead);

                builder->SetInsertPoint(loopExit);
            }

            return hdrPtr;
        };

        return emit(0);
    }

} // code
