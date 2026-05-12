//
// Created by James Klappenbach on 4/14/23.
//

#include "Identifier.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/error/Exception.h"

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
        // Use-after-move check: if this identifier has been transferred via `#`
        // earlier in the active scope, reject the read at compile time.
        // Per `MemoryModel.md` § Static analysis rules § Use-after-move.
        auto scope = module->getScopeStack().peek();
        if (scope && scope->isMoved(identifier)) {
            throw Exception("use-after-move: identifier '" + identifier
                + "' was transferred via `#` and cannot be read here",
                "CAJETA_ERROR_USE_AFTER_MOVE");
        }
        // Identifier always resolves to a local field's address (its alloca). Member access
        // (`obj.member`) is the responsibility of DotExpression, not this node — the legacy
        // `primary == false` branch was speculative and never reached.
        FieldPtr field = scope ? scope->getField(identifier) : nullptr;
        return field ? static_cast<llvm::Value*>(field->getOrCreateAllocation()) : nullptr;
    }

} // code