//
// Created by James Klappenbach on 3/22/23.
//

#include "StackField.h"

namespace cajeta {
    llvm::Value* StackField::createLoad() {
        return nullptr;
    }
    llvm::Value* StackField::createStore(llvm::Value* value) {
        return nullptr;
    }
    llvm::AllocaInst* StackField::getOrCreateAllocation() {
        return nullptr;
    }
} // cajeta