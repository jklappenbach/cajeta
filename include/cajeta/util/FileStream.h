//
// Created by James Klappenbach on 11/15/22.
//

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <mutex>
#include <string>

using namespace std;

namespace cajeta {

    /**
     * The file descriptors for stdin, stdout, and stderr are 0, 1, and 2, respectively.
     */
    class FileStream {
    protected:
        static std::mutex mutex;
        static FileStream* theInstance;
        llvm::StructType* file;
        llvm::FunctionType* openFunctionType;
        llvm::Function* openFunction;
        llvm::FunctionType* descOpenFunctionType;
        llvm::Function* descOpenFunction;
        llvm::FunctionType* closeFunctionType;
        llvm::Function* closeFunction;
        llvm::FunctionType* controlFunctionType;
        llvm::Function* controlFunction;
        llvm::FunctionType* flushFunctionType;
        llvm::Function* flushFunction;
        llvm::FunctionType* seekFunctionType;
        llvm::Function* seekFunction;
        llvm::FunctionType* rewindFunctionType;
        llvm::Function* rewindFunction;
        llvm::FunctionType* tellFunctionType;
        llvm::Function* tellFunction;
        llvm::FunctionType* statusFunctionType;
        llvm::Function* statusFunction;
        llvm::FunctionType* getPositionFunctionType;
        llvm::Function* getPositionFunction;
        llvm::FunctionType* setPositionFunctionType;
        llvm::Function* setPositionFunction;

        FileStream();

    public:
        static FileStream* getInstance();

        llvm::CallInst* createOpenInstruction(llvm::Value* file, llvm::Constant* openFlags,
            llvm::BasicBlock* basicBlock);

        llvm::CallInst* createOpenInstruction(llvm::Constant* path, llvm::Constant* openFlags,
            llvm::BasicBlock* basicBlock);

        llvm::CallInst* createCloseInstruction(llvm::Value* file, llvm::Constant* openFlags,
            llvm::BasicBlock* basicBlock);

        llvm::CallInst* createControlInstruction(llvm::Value* file, llvm::Constant* openFlags,
            llvm::BasicBlock* basicBlock);

        llvm::CallInst* createFlushInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock);

        llvm::CallInst* createSeekInstruction(llvm::Value* file, llvm::Constant* offset, llvm::Constant* whence,
            llvm::BasicBlock* basicBlock);

        llvm::CallInst* createRewindInstruction(llvm::Value* file, llvm::Constant* offset, llvm::Constant* whence,
            llvm::BasicBlock* basicBlock);

        llvm::CallInst* createTellInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock);

        llvm::CallInst* createStatusInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock);

        llvm::CallInst* createGetPositionInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock);

        llvm::CallInst* createSetPositionInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock);
    };

} // cajeta