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

        // Initialize the vtable pointer at instance slot 0. Required for
        // dynamic dispatch — `dog.speak()` reads slot 0 to find Dog's vtable
        // before binary-searching for `speak`'s hash. Without this write the
        // instance's vtable pointer is whatever malloc returned (uninitialized
        // bytes), and the first virtual call segfaults.
        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            if (llvm::GlobalVariable* vtable = klass->getVirtualTableGlobal()) {
                llvm::Value* vtablePtrSlot = builder->CreateStructGEP(
                    structTy, instance, /*idx=*/0, "vtable_slot");
                builder->CreateStore(vtable, vtablePtrSlot);
            }
        }

        // Resolve parameters and call the constructor.
        vector<ParameterEntry> entries;
        for (auto& param : parameters) {
            llvm::Value* value = param.expression->generateCode(module);
            entries.push_back(ParameterEntry(CajetaType::of(value), param.label, value));
        }
        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            string ctorName = targetType->getQName()->getTypeName();
            klass->invokeMethod(ctorName, entries, /*isConstructor=*/true, instance);
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
