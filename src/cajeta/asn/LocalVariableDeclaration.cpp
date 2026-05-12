//
// Created by James Klappenbach on 11/4/22.
//

#include "LocalVariableDeclaration.h"
#include "../compile/CajetaModule.h"
#include "../field/HeapField.h"
#include "../field/StackField.h"
#include "../type/CajetaArray.h"
#include "../error/CajetaExceptions.h"
#include "../logging/CajetaLogger.h"

namespace cajeta {
    /**
     * If we have a primitive variable, we can store in on the stack and will immediately create an currentRegister.
     * Otherwise, we will create an currentRegister for a structure reference.  If the variable receives a new operator,
     * we'll just let the malloc call create the register
     *
     * @param module
     * @return
     */
    llvm::Value* LocalVariableDeclaration::generateCode(CajetaModulePtr module) {

        // Arrays and class instances live on the heap; their local slot is a pointer.
        // Only true primitives (int32, float64, bool, etc.) get an inline-value alloca.
        bool isArray = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
        bool wantsInlineSlot = (type->getTypeFlags() & PRIMITIVE_FLAG) && !isArray;
        for (auto& declarator: variableDeclarators) {
            InitializerPtr initializer = declarator->getInitializer();
            FieldPtr field;
            if (wantsInlineSlot) {
                field = make_shared<StackField>(module, declarator->getIdentifier(), type,
                    declarator->isReference(), modifiers, annotations, initializer);
            } else {
                field = make_shared<HeapField>(module, declarator->getIdentifier(), type,
                    declarator->isReference(), modifiers, annotations, initializer);
            }
            module->getScopeStack().peek()->putField(field);
            field->getOrCreateAllocation();
        }

        return nullptr;
    }

} // code