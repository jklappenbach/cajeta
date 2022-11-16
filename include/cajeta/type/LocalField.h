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
        LocalField(string name, CajetaType* type, int arrayDimension,
                bool reference, set<Modifier> modifiers, set<QualifiedName*> annotations, Initializer* initializer) :
                Field(name, type, arrayDimension, reference, modifiers, annotations, initializer) { }

        bool isVar() const {
            return var;
        }


    };

} // cajeta