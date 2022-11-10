//
// Created by James Klappenbach on 11/4/22.
//

#include <cajeta/asn/LocalVariableDeclaration.h>
#include <cajeta/compile/CajetaModule.h>
#include <cajeta/type/Field.h>
#include <cajeta/exception/CajetaExceptions.h>
#include <cajeta/logging/CajetaLogger.h>

namespace cajeta {
    llvm::Value* LocalVariableDeclaration::generateCode(CajetaModule* module) {
        for (auto &declarator : variableDeclarators) {
            Initializer* initializer = declarator->getInitializer();
            Field* field = new Field(declarator->getIdentifier(), type, declarator->getArrayDimension(),
                                     declarator->isReference(), modifiers, initializer);
            try {
                module->getCurrentMethod()->createLocalVariable(module, field);
            } catch (VariableCreationException& e) {
                CajetaLogger::log(ERROR, module, this, e);
            }
        }

        return nullptr;
    }

} // cajeta