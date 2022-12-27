//
// Created by James Klappenbach on 11/9/22.
//

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <mutex>
#include <string>
#include <map>

using namespace std;

namespace cajeta {
    class CajetaModule;

    class Printer {
    private:
        static llvm::FunctionCallee getPrintf(CajetaModule* module);
    public:
        /**
         *
         * @param format
         * @param printArgs
         * @param basicBlock
         * @return
         */
        static llvm::CallInst* createPrintfInstruction(CajetaModule* module, vector<llvm::Value*> printArgs,
                llvm::BasicBlock* basicBlock);

    };
}
