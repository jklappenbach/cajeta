//
// Created by James Klappenbach on 4/14/23.
//

#include "Identifier.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* IdentifierExpression::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());

        if (primary) {
            FieldPtr field = module->getScopeStack().peek()->getField(identifier);
            return field->getOrCreateAllocation();
        } else {
            // TODO: Fix Me!
//            llvm::Value* value = pModule->getFieldStack().back()->getOrCreateAllocation();
//            CajetaStructurePtr structure;
//            try {
//                structure = static_pointer_cast<CajetaStructure>(CajetaType::getCanonicalMap()[value->getType()->getStructName().str()]);
//            } catch (exception) {
//                throw "bad type";
//            }
//            StructurePropertyPtr structureField = structure->getProperties()[identifier];
//            return pModule->getBuilder()->CreateStructGEP(structureField->getType()->getLlvmType(),
//                pModule->getFieldStack().back()->getOrCreateAllocation(),
//                structureField->getOrder(),
//                identifier);
        }
        module->getAsnStack().pop_back();
        return nullptr;
    }

} // code