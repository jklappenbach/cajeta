//
// Created by James Klappenbach on 11/4/22.
//

#include <cajeta/asn/LocalVariableDeclaration.h>
#include <cajeta/compile/CajetaModule.h>
#include <cajeta/type/Field.h>
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
        for (auto &declarator : variableDeclarators) {
            Initializer* initializer = declarator->getInitializer();
            Field* field = new Field(declarator->getIdentifier(), type, declarator->getArrayDimension(),
                                     declarator->isReference(), modifiers, initializer);
            module->setCurrentField(field);
            module->getCurrentMethod()->createLocalVariable(module, field);
        }

        return nullptr;
    }

} // cajeta