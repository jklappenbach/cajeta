//
// Created by James Klappenbach on 4/14/23.
//

#include "Identifier.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    void IdentifierExpression::resolveTypes(CajetaModulePtr module) {
        // Look up the identifier in the active scope and pin our resolvedType to the
        // referenced field's type. Used downstream by DotExpression and ArrayIndexExpression.
        if (!module->getScopeStack().isEmpty()) {
            FieldPtr field = module->getScopeStack().peek()->getField(identifier);
            if (field) {
                resolvedType = field->getType();
            }
        }
    }

    llvm::Value* IdentifierExpression::generateCode(CajetaModulePtr module) {
        // Identifier always resolves to a local field's address (its alloca). Member access
        // (`obj.member`) is the responsibility of DotExpression, not this node — the legacy
        // `primary == false` branch was speculative and never reached.
        FieldPtr field = module->getScopeStack().peek()->getField(identifier);
        return field ? static_cast<llvm::Value*>(field->getOrCreateAllocation()) : nullptr;
    }

} // code