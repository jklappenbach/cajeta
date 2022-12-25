//
// Created by James Klappenbach on 11/14/22.
//

#pragma once

#include "cajeta/field/Field.h"

namespace cajeta {

    class LocalField : public Field {
    protected:
        bool var;
    public:
        LocalField(string name, CajetaType* type, bool reference, set<Modifier> modifiers,
            set<QualifiedName*> annotations, Initializer* initializer, Field* parent = nullptr) :
            Field(name, type, reference, modifiers, annotations, initializer, parent) { }

        LocalField(string name, CajetaType* type, llvm::Value* allocation, Field* parent = nullptr) : Field(name, type, allocation, parent) { }

        LocalField(string name, CajetaType* type, Field* parent = nullptr) : Field(name, type, parent) { }

        bool isVar() const {
            return var;
        }
    };

} // cajeta