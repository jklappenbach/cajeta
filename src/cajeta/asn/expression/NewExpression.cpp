//
// Created by James Klappenbach on 4/19/23.
//

#include "NewExpression.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* NewExpression::generateCode(CajetaModulePtr module) {
        if (!creatorRest) {
            return nullptr;
        }
        // Look up the target type by name. typeName names the class for `new Foo()`, or
        // the element type for `new T[...]`. Package is "" for primitives (e.g. int32).
        CajetaTypePtr type = CajetaType::of(typeName, package);
        if (!type) {
            // Fallback to canonical lookup by bare typeName for primitives.
            type = CajetaType::of(typeName);
        }
        creatorRest->setTargetType(type);
        return creatorRest->generateCode(module);
    }
} // code