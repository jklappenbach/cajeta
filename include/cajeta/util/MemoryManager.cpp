//
// Created by James Klappenbach on 11/9/22.
//

#include "MemoryManager.h"
#include <cajeta/compile/CajetaModule.h>

namespace cajeta {

    llvm::FunctionCallee MemoryManager::getMalloc(CajetaModule* module) {
        llvm::Function* fn = module->getLlvmModule()->getFunction("malloc");
        llvm::FunctionCallee fnCallee;

        if (!fn) {
            llvm::Type* sizeArgumentType = llvm::Type::getInt64Ty(*module->getLlvmContext());
            llvm::Type* returnType = llvm::Type::getInt64PtrTy(*module->getLlvmContext());
            std::vector<llvm::Type*> arguments;
            arguments.push_back(sizeArgumentType);
            llvm::FunctionType* fnType = llvm::FunctionType::get(returnType, llvm::ArrayRef<llvm::Type*>(arguments), false);
            fnCallee = module->getLlvmModule()->getOrInsertFunction("malloc", fnType);
        } else {
            fnCallee = module->getLlvmModule()->getOrInsertFunction("malloc", fn->getFunctionType());
        }
        return fnCallee;
    }

    llvm::FunctionCallee MemoryManager::getFree(cajeta::CajetaModule* module) {
        llvm::Function* fn = module->getLlvmModule()->getFunction("free");
        llvm::FunctionCallee fnCallee;

        if (!fn) {
            llvm::Type* returnType = llvm::Type::getVoidTy(*module->getLlvmContext());
            llvm::Type* pointerType = llvm::Type::getInt64PtrTy(*module->getLlvmContext());
            std::vector<llvm::Type*> arguments;
            arguments.push_back(pointerType);
            llvm::FunctionType* fnType = llvm::FunctionType::get(returnType, llvm::ArrayRef<llvm::Type*>(arguments), false);
            fnCallee = module->getLlvmModule()->getOrInsertFunction("free", fnType);
        } else {
            fnCallee = module->getLlvmModule()->getOrInsertFunction("free", fn->getFunctionType());
        }
        return fnCallee;
    }

    llvm::CallInst* MemoryManager::createMallocInstruction(CajetaModule* module, string registerName,
        llvm::Constant* allocSize,
        llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(allocSize);
        return llvm::CallInst::Create(getMalloc(module),
            llvm::ArrayRef<llvm::Value*>(args),
            registerName,
            basicBlock);
    }

    llvm::CallInst* MemoryManager::createMallocInstruction(CajetaModule* module, llvm::Constant* allocSize,
            llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(allocSize);
        return llvm::CallInst::Create(getMalloc(module),
            llvm::ArrayRef<llvm::Value*>(args),
            "",
            basicBlock);
    }

    llvm::CallInst* MemoryManager::createFreeInstruction(CajetaModule* module, llvm::Value* pointer,
            llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(pointer);
        return llvm::CallInst::Create(getFree(module),
            llvm::ArrayRef<llvm::Value*>(args),
            "",
            basicBlock);
    }

}
