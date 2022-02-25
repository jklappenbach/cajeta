//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <cajeta/TypeDefinition.h>

using namespace std;

namespace cajeta {
    struct Field {
        bool reference;
        string name;
        string typeName;
        TypeDefinition* typeDefinition;
        llvm::AllocaInst* definition;
        llvm::Type* type;
        void define() { }
        void allocate(llvm::IRBuilder<>* builder, llvm::Module* module, llvm::LLVMContext* llvmContext) {
            // typeDefinition->allocate(builder, module, llvmContext);
        }
        void release(llvm::IRBuilder<>* builder, llvm::Module* module, llvm::LLVMContext* llvmContext) {
            // typeDefinition->free(builder, module, llvmContext);
        }
    };
}