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
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class Printer {
    private:
        static llvm::FunctionCallee getPrintf(CajetaModulePtr module);

    public:
        /**
         *
         * @param format
         * @param printArgs
         * @param basicBlock
         * @return
         */
        static llvm::CallInst* createPrintfInstruction(CajetaModulePtr module, vector<llvm::Value*> printArgs,
            llvm::BasicBlock* basicBlock);

    };
}
