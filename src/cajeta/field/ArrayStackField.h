//
// Created by James Klappenbach on 3/22/23.
//

#pragma once
#include "cajeta/field/Field.h"

namespace cajeta {

    class StackField : public Field {
        llvm::Value* createLoad() override;
        llvm::Value* createStore(llvm::Value* value) override;
        llvm::Value* getOrCreateAllocation() override;
    };

} // cajeta