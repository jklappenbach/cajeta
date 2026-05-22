//
// Created by James Klappenbach on 11/9/22.
//

#include "MemoryManager.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    llvm::FunctionCallee MemoryManager::getMalloc(CajetaModulePtr module) {
        llvm::Function* fn = module->getLlvmModule()->getFunction("malloc");
        llvm::FunctionCallee fnCallee;

        if (!fn) {
            llvm::Type* sizeArgumentType = llvm::Type::getInt64Ty(*module->getLlvmContext());
            llvm::Type* returnType = llvm::PointerType::get(*module->getLlvmContext(), 0);
            std::vector<llvm::Type*> arguments;
            arguments.push_back(sizeArgumentType);
            llvm::FunctionType* fnType = llvm::FunctionType::get(returnType, llvm::ArrayRef<llvm::Type*>(arguments),
                false);
            fnCallee = module->getLlvmModule()->getOrInsertFunction("malloc", fnType);
        } else {
            fnCallee = module->getLlvmModule()->getOrInsertFunction("malloc", fn->getFunctionType());
        }
        return fnCallee;
    }

    llvm::FunctionCallee MemoryManager::getFree(cajeta::CajetaModulePtr module) {
        llvm::Function* fn = module->getLlvmModule()->getFunction("free");
        llvm::FunctionCallee fnCallee;

        if (!fn) {
            llvm::Type* returnType = llvm::Type::getVoidTy(*module->getLlvmContext());
            llvm::Type* pointerType = llvm::PointerType::get(*module->getLlvmContext(), 0);
            std::vector<llvm::Type*> arguments;
            arguments.push_back(pointerType);
            llvm::FunctionType* fnType = llvm::FunctionType::get(returnType, llvm::ArrayRef<llvm::Type*>(arguments),
                false);
            fnCallee = module->getLlvmModule()->getOrInsertFunction("free", fnType);
        } else {
            fnCallee = module->getLlvmModule()->getOrInsertFunction("free", fn->getFunctionType());
        }
        return fnCallee;
    }

    // After every malloc, register the result with the live-allocation set
    // so the auto field-drop scheme (FieldOwnership.md § Solution B) can
    // distinguish "this address is live, drop it" from "already freed by
    // an aliasing owner, no-op." Without this, class instances allocated
    // via libc malloc directly never appear in the set and auto-drop
    // silently skips them.
    static void emitLiveSetAdd(CajetaModulePtr module, llvm::CallInst* malloc, llvm::BasicBlock* basicBlock) {
        // --live-set=off (release/minimal builds) skips registration
        // entirely. Bounded and Strict both register; the runtime side
        // distinguishes them (Strict asserts on duplicates, Bounded
        // caps capacity). Off means class instances allocated via
        // libc malloc never enter the live-set, so the auto field-
        // drop pass becomes a no-op on them — caller code that owns
        // those references is responsible for explicit cleanup.
        if (module->getFlags().liveSet == LiveSet::Off) return;
        llvm::Function* addFn = module->getRuntimeFunction("__cajeta_live_set_add");
        if (!addFn) return;
        std::vector<llvm::Value*> args = {malloc};
        llvm::CallInst::Create(addFn, llvm::ArrayRef<llvm::Value*>(args),
            "", basicBlock);
    }

    llvm::CallInst* MemoryManager::createMallocInstruction(CajetaModulePtr module, string registerName,
        llvm::Constant* allocSize,
        llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(allocSize);
        llvm::CallInst* malloc = llvm::CallInst::Create(getMalloc(module),
            llvm::ArrayRef<llvm::Value*>(args),
            registerName,
            basicBlock);
        emitLiveSetAdd(module, malloc, basicBlock);
        return malloc;
    }

    llvm::CallInst* MemoryManager::createMallocInstruction(CajetaModulePtr module, llvm::Constant* allocSize,
        llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(allocSize);
        llvm::CallInst* malloc = llvm::CallInst::Create(getMalloc(module),
            llvm::ArrayRef<llvm::Value*>(args),
            "",
            basicBlock);
        emitLiveSetAdd(module, malloc, basicBlock);
        return malloc;
    }

    llvm::CallInst* MemoryManager::createFreeInstruction(CajetaModulePtr module, llvm::Value* pointer,
        llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(pointer);
        return llvm::CallInst::Create(getFree(module),
            llvm::ArrayRef<llvm::Value*>(args),
            "",
            basicBlock);
    }
}
