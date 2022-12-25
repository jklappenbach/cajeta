//
// Created by James Klappenbach on 11/14/22.
//

#include "LocalPropertyField.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* LocalPropertyField::createLoad(CajetaModule* module) {
        llvm::Value* value = parent->createLoad(module);
        value = module->getBuilder()->CreateStructGEP(type->getLlvmType(), value, index);
        return module->getBuilder()->CreateLoad(type->getLlvmType(), value);
    }
} // cajeta