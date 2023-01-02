//
// Created by James Klappenbach on 2/20/22.
//

#include "cajeta/field/LocalStructureField.h"
#include "cajeta/asn/VariableDeclarator.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/Scope.h"

namespace cajeta {

    llvm::Value* LocalStructureField::getOrCreateAllocation(CajetaModule* module) {
        if (!origin) {
            origin = module->getBuilder()->CreateAlloca(type->getLlvmType()->getPointerTo());
            if (initializer) {
                module->getFieldStack().push_back(this);
                initializer->generateCode(module);
                module->getFieldStack().pop_back();
            }
        }
        return origin;
    }

    llvm::Value* LocalStructureField::createLoad(CajetaModule* module) {
        if (origin == nullptr) {
            origin = this->getOrCreateAllocation(module);
        }
        return module->getBuilder()->CreateLoad(type->getLlvmType()->getPointerTo(), origin);
    }

    void LocalStructureField::onDelete(CajetaModule* module, Scope* scope) {
        MemoryManager::createFreeInstruction(module, createLoad(module),
            module->getBuilder()->GetInsertBlock());
    }
}
