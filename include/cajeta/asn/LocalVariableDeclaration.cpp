//
// Created by James Klappenbach on 11/4/22.
//

#include <cajeta/asn/LocalVariableDeclaration.h>
#include <cajeta/compile/CajetaModule.h>
#include <cajeta/type/LocalField.h>
#include <cajeta/exception/CajetaExceptions.h>
#include <cajeta/logging/CajetaLogger.h>

namespace cajeta {
    /**
     * If we have a primitive variable, we can store in on the stack and will immediately create an allocation.
     * Otherwise, we will create an allocation for a structure reference.  If the variable receives a new operator,
     * we'll just let the malloc call create the register
     *
     * @param module
     * @return
     */
    llvm::Value* LocalVariableDeclaration::generateCode(CajetaModule* module) {
        for (auto& declarator: variableDeclarators) {
            Initializer* initializer = declarator->getInitializer();
            LocalField* field;
            if (type->getStructType() == STRUCT_TYPE_PRIMITIVE) {
                field = new LocalField(declarator->getIdentifier(), type,
                    declarator->isReference(), modifiers, annotations, initializer);
            } else {
                field = new LocalField(declarator->getIdentifier(), ((CajetaStructure*)type)->getPointerType(),
                    declarator->isReference(), modifiers, annotations, initializer);
            }
            module->getTypeStack().push_back(type);
            module->getFieldStack().push_back(field);
            module->getScopeStack().back()->putField(field);
            module->getFieldStack().pop_back();
        }

        return nullptr;
    }

} // cajeta