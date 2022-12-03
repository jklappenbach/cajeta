//
// Created by James Klappenbach on 11/14/22.
//

#pragma once

#include <cajeta/type/Field.h>

namespace cajeta {

    class LocalField : public Field {
    protected:
        bool var;
    public:
        LocalField(string name, CajetaType* type, bool reference, set<Modifier> modifiers,
            set<QualifiedName*> annotations, Initializer* initializer) :
            Field(name, type, reference, modifiers, annotations, initializer) { }

        LocalField(string name, CajetaType* type, llvm::Value* allocation) : Field(name, type, allocation) { }

        bool isVar() const {
            return var;
        }
    };

} // cajeta