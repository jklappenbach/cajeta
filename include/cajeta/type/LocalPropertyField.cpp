//
// Created by James Klappenbach on 11/14/22.
//

#include "LocalPropertyField.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* LocalPropertyField::createLoad(CajetaModule* module) {
        llvm::Value* value;
        if (parent) {
            value = parent->createLoad(module);
        } else {
            value = allocation;
        }
        return module->getBuilder()->CreateStructGEP(type->getLlvmType(), value, index);
    }
} // cajeta