//
// Created by James Klappenbach on 11/9/22.
//

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <mutex>
#include <cajeta/compile/CajetaModule.h>

namespace cajeta {
    class MemoryAllocator {
    private:
        static std::mutex mutex;
        static MemoryAllocator* theInstance;
        llvm::FunctionType* functionType;
        llvm::FunctionCallee functionCallee;
        int x;
    public:

        /**
         *
         * @param module
         * @return
         */
        static MemoryAllocator* getInstance(CajetaModule* module);

        /**
         *
         * @param module
         */
        MemoryAllocator(CajetaModule* module) {
            llvm::Type* sizeArgumentType = llvm::Type::getInt64Ty(*module->getLlvmContext());
            llvm::Type* returnType = llvm::Type::getInt64PtrTy(*module->getLlvmContext());
            std::vector<llvm::Type*> arguments;
            arguments.push_back(sizeArgumentType);
            functionType = llvm::FunctionType::get(returnType, llvm::ArrayRef<llvm::Type*>(arguments), false);
            functionCallee = module->getLlvmModule()->getOrInsertFunction("malloc", functionType);
        }

        /**
         *
         * @param allocSize
         * @param args
         * @param basicBlock
         * @return
         */
        llvm::CallInst* createMallocInstruction(llvm::Constant* allocSize,
                                                llvm::BasicBlock* basicBlock) {
            vector<llvm::Value*> args;
            args.push_back(allocSize);
            return llvm::CallInst::Create(functionCallee, llvm::ArrayRef<llvm::Value*>(args), "malloc", basicBlock);
        }
    };
}
