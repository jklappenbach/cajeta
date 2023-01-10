//
// Created by James Klappenbach on 11/15/22.
//

#include "FileStream.h"

namespace cajeta {
    std::mutex FileStream::mutex;
    FileStream* FileStream::theInstance = nullptr;

    FileStream::FileStream() {

    }

    FileStream* FileStream::getInstance() {
        mutex.lock();
        if (!theInstance) {
            theInstance = new FileStream;
        }
        mutex.unlock();
        return theInstance;
    }

    llvm::CallInst* FileStream::createOpenInstruction(llvm::Value* file, llvm::Constant* openFlags,
        llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst* FileStream::createOpenInstruction(llvm::Constant* path, llvm::Constant* openFlags,
        llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst* FileStream::createCloseInstruction(llvm::Value* file, llvm::Constant* openFlags,
        llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst* FileStream::createControlInstruction(llvm::Value* file, llvm::Constant* openFlags,
        llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst* FileStream::createFlushInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst* FileStream::createSeekInstruction(llvm::Value* file, llvm::Constant* offset, llvm::Constant* whence,
        llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst*
    FileStream::createRewindInstruction(llvm::Value* file, llvm::Constant* offset, llvm::Constant* whence,
        llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst* FileStream::createTellInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst* FileStream::createStatusInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst* FileStream::createGetPositionInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock) {
        return nullptr;
    }

    llvm::CallInst* FileStream::createSetPositionInstruction(llvm::Value* file, llvm::BasicBlock* basicBlock) {
        return nullptr;
    }
} // cajeta