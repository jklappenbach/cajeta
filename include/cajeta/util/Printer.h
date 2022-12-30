//
// Created by James Klappenbach on 11/9/22.
//

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <mutex>
#include <string>

using namespace std;

namespace cajeta {
    class CajetaModule;

    class Printer {
    private:
        static std::mutex mutex;
        static Printer* theInstance;
        llvm::FunctionType* printerFunctionType;
        llvm::FunctionCallee printerFunctionCallee;

        Printer(CajetaModule* module);
        void init(CajetaModule* module);
    public:

        /**
         *
         * @param module
         * @return
         */
        static Printer* getInstance(CajetaModule* module);

        /**
         *
         * @param format
         * @param printArgs
         * @param basicBlock
         * @return
         */
        llvm::CallInst* createPrintfInstruction(llvm::ConstantDataArray* format, vector<llvm::ConstantDataArray*> printArgs,
                llvm::BasicBlock* basicBlock);

        /**
         *
         * @param format
         * @param printArgs
         * @param basicBlock
         * @return
         */
        llvm::CallInst* createPrintfInstruction(llvm::Value* format, vector<llvm::Value*> printArgs,
                llvm::BasicBlock* basicBlock);

    };
}
