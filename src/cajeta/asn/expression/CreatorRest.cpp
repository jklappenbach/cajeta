//
// Created by James Klappenbach on 4/19/23.
//

#include "CreatorRest.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/util/MemoryManager.h"

#include <functional>

namespace cajeta {
    shared_ptr<CreatorRest> CreatorRest::fromContext(CajetaParser::CreatorContext* ctx, antlr4::Token* token) {
        if (ctx->classCreatorRest()) {
            return make_shared<ClassCreatorRest>(ctx->classCreatorRest(), token);
        } else {
            return make_shared<ArrayCreatorRest>(ctx->arrayCreatorRest(), token);
        }
    }

    // Emits `malloc(sizeof(struct))` then dispatches to the matching constructor with the
    // user-supplied arguments. Returns the malloc'd pointer.
    llvm::Value* ClassCreatorRest::generateCode(CajetaModulePtr module) {
        if (!targetType) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        llvm::Type* structTy = targetType->getLlvmType();
        const llvm::DataLayout& dataLayout = module->getLlvmModule()->getDataLayout();
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(*module->getLlvmContext()),
            dataLayout.getTypeAllocSize(structTy));
        llvm::CallInst* instance = MemoryManager::createMallocInstruction(
            module, allocSize, builder->GetInsertBlock());

        // S7.2 — zero-init the heap block. malloc returns uninitialized
        // bytes; the vtable slot below gets overwritten, and most fields
        // get written by the user's constructor body. But fields the ctor
        // doesn't explicitly initialize stay at whatever malloc gave us —
        // garbage. That's load-bearing now for class-drop recursion into
        // embedded struct fields (S7.2): the recursive struct drop reads
        // class-ref slots inside the embedded struct, and a non-null
        // garbage pointer slips past the Tracer-drop null guard and
        // crashes on free(). Zero-init matches JVM / .NET defaults too;
        // class-ref fields read as null until the ctor writes them.
        builder->CreateMemSet(instance,
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(*module->getLlvmContext()), 0),
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
