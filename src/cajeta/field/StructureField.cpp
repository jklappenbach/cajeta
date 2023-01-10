//
// Created by James Klappenbach on 11/14/22.
//

#include "StructureField.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* StructureField::createLoad() {
        llvm::Value* value = parent->createLoad();
        value = module->getBuilder()->CreateStructGEP(parent->getType()->getLlvmType(), value, index);
        return module->getBuilder()->CreateLoad(type->getLlvmType(), value);
    }
} // cajeta