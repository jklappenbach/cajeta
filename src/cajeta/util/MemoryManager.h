// MemoryManager - the malloc/free call sites codegen emits.

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <string>

using namespace std;

namespace cajeta {
    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class MemoryManager {
    private:
        /** libc `malloc` in `module`, declared `ptr(i64)` on first use and reused
         *  after; a declaration already present keeps its own type. */
        static llvm::FunctionCallee getMalloc(CajetaModulePtr module);
        /** libc `free` in `module`, declared `void(ptr)` on first use and reused
         *  after; a declaration already present keeps its own type. */
        static llvm::FunctionCallee getFree(CajetaModulePtr module);

    public:
        /** A malloc of `allocSize` bytes at the end of `basicBlock`, its result
         *  named `registerName`. */
        static llvm::CallInst*  createMallocInstruction(CajetaModulePtr module, string registerName, llvm::Constant* allocSize,
            llvm::BasicBlock* basicBlock);

        /** The same, leaving the result register unnamed. */
        static llvm::CallInst* createMallocInstruction(CajetaModulePtr module, llvm::Constant* allocSize,
            llvm::BasicBlock* basicBlock);

        /** A free of `pointer` at the end of `basicBlock`. */
        static llvm::CallInst* createFreeInstruction(CajetaModulePtr module, llvm::Value* pointer,
            llvm::BasicBlock* basicBlock);

    };
}
