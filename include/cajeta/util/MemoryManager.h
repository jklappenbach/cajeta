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

    class MemoryManager {
    private:

        static llvm::FunctionCallee getMalloc(CajetaModule* module);
        static llvm::FunctionCallee getFree(CajetaModule* module);

    public:


        /**
         *
         * @param allocSize
         * @param args
         * @param basicBlock
         * @return
         */
        static llvm::CallInst* createMallocInstruction(CajetaModule* module, string registerName, llvm::Constant* allocSize,
            llvm::BasicBlock* basicBlock);

        /**
         *
         * @param allocSize
         * @param args
         * @param basicBlock
         * @return
         */
        static llvm::CallInst* createMallocInstruction(CajetaModule* module, llvm::Constant* allocSize,
            llvm::BasicBlock* basicBlock);

        /**
         *
         * @param pointer
         * @param basicBlock
         * @return
         */
        static llvm::CallInst* createFreeInstruction(CajetaModule* module, llvm::Value* pointer,
            llvm::BasicBlock* basicBlock);

    };
}
