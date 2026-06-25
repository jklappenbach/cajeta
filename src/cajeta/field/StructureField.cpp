//
// Created by James Klappenbach on 11/14/22.
//

#include "StructureField.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* StructureField::createLoad() {
        llvm::Value* value = parent->createLoad();
        value = module->getBuilder()->CreateStructGEP(parent->getType()->getLlvmType(), value, index);
        // TBAA: object-field access (see CajetaModule TBAA section).
        module->recordTbaaProvenance(value, CajetaModule::TbaaKind::Field);
        return module->getBuilder()->CreateLoad(type->getLlvmType(), value);
    }
} // code