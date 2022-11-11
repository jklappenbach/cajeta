//
// Created by James Klappenbach on 11/9/22.
//

#include "MemoryManager.h"
#include <cajeta/compile/CajetaModule.h>

namespace cajeta {
    std::mutex MemoryManager::mutex;
    MemoryManager* MemoryManager::theInstance;

    MemoryManager::MemoryManager(CajetaModule* module) {
        initMalloc(module);
        initFree(module);
    }

    MemoryManager* MemoryManager::getInstance(CajetaModule* module) {
        mutex.lock();
        if (!theInstance) {
            theInstance = new MemoryManager(module);
        }
        mutex.unlock();
        return theInstance;
    }

    void MemoryManager::initMalloc(CajetaModule* module) {
        llvm::Type* sizeArgumentType = llvm::Type::getInt64Ty(*module->getLlvmContext());
        llvm::Type* returnType = llvm::Type::getInt64PtrTy(*module->getLlvmContext());
        std::vector<llvm::Type*> arguments;
        arguments.push_back(sizeArgumentType);
        mallocFunctionType = llvm::FunctionType::get(returnType, llvm::ArrayRef<llvm::Type*>(arguments), false);
        mallocFunctionCallee = module->getLlvmModule()->getOrInsertFunction("malloc", mallocFunctionType);
    }

    void MemoryManager::initFree(CajetaModule* module) {
        llvm::Type* returnType = llvm::Type::getVoidTy(*module->getLlvmContext());
        llvm::Type* pointerType = llvm::Type::getInt64PtrTy(*module->getLlvmContext());
        std::vector<llvm::Type*> arguments;
        arguments.push_back(pointerType);
        freeFunctionType = llvm::FunctionType::get(returnType, llvm::ArrayRef<llvm::Type*>(arguments), false);
        freeFunctionCallee = module->getLlvmModule()->getOrInsertFunction("free", freeFunctionType);
    }

    llvm::CallInst* MemoryManager::createMallocInstruction(string registerName, llvm::Constant* allocSize,
                                            llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(allocSize);
        return llvm::CallInst::Create(mallocFunctionCallee,
                                      llvm::ArrayRef<llvm::Value*>(args),
                                      registerName,
                                      basicBlock);
    }


    llvm::CallInst* MemoryManager::createFreeInstruction(llvm::Value* pointer,
                                          llvm::BasicBlock* basicBlock) {
        vector<llvm::Value*> args;
        args.push_back(pointer);
        return llvm::CallInst::Create(freeFunctionCallee,
                                      llvm::ArrayRef<llvm::Value*>(args),
                                      "",
                                      basicBlock);
    }

}
