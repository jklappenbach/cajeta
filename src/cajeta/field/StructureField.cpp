#include "StructureField.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    // Loads the field: GEP the parent's storage at this field's index, then load it.
    // The address is tagged with field-kind TBAA provenance before the load.
    llvm::Value* StructureField::createLoad() {
        llvm::Value* value = parent->createLoad();
        value = module->getBuilder()->CreateStructGEP(parent->getType()->getLlvmType(), value, index);
        module->recordTbaaProvenance(value, CajetaModule::TbaaKind::Field);
        return module->getBuilder()->CreateLoad(type->getLlvmType(), value);
    }
} // code