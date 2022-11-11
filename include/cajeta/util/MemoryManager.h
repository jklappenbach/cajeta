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
        static std::mutex mutex;
        static MemoryManager* theInstance;
        llvm::FunctionType* mallocFunctionType;
        llvm::FunctionCallee mallocFunctionCallee;
        llvm::FunctionType* freeFunctionType;
        llvm::FunctionCallee freeFunctionCallee;

        void initMalloc(CajetaModule* module);
        void initFree(CajetaModule* module);

    public:

        /**
         *
         * @param module
         * @return
         */
        static MemoryManager* getInstance(CajetaModule* module);

        /**
         *
         * @param module
         */
        MemoryManager(CajetaModule* module);

        /**
         *
         * @param allocSize
         * @param args
         * @param basicBlock
         * @return
         */
        llvm::CallInst* createMallocInstruction(string registerName, llvm::Constant* allocSize,
                                                llvm::BasicBlock* basicBlock);


        llvm::CallInst* createFreeInstruction(llvm::Value* pointer,
                                              llvm::BasicBlock* basicBlock);

    };
}
