//
// Created by James Klappenbach on 11/9/22.
//

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <mutex>
#include <string>

using namespace std;

namespace cajeta {
    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class MemoryManager {
    private:
        static llvm::FunctionCallee getMalloc(CajetaModulePtr module);
        static llvm::FunctionCallee getFree(CajetaModulePtr module);

    public:
        /**
         *
         * @param allocSize
         * @param args
         * @param basicBlock
         * @return
         */
        static llvm::CallInst*  createMallocInstruction(CajetaModulePtr module, string registerName, llvm::Constant* allocSize,
            llvm::BasicBlock* basicBlock);

        /**
         *
         * @param allocSize
         * @param args
         * @param basicBlock
         * @return
         */
        static llvm::CallInst* createMallocInstruction(CajetaModulePtr module, llvm::Constant* allocSize,
            llvm::BasicBlock* basicBlock);

        /**
         *
         * @param pointer
         * @param basicBlock
         * @return
         */
        static llvm::CallInst* createFreeInstruction(CajetaModulePtr module, llvm::Value* pointer,
            llvm::BasicBlock* basicBlock);

    };
}
