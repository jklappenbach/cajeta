//
// Created by James Klappenbach on 11/14/22.
//

#pragma once

#include <cajeta/type/Field.h>

namespace cajeta {

    class StructureField : public Field {
    protected:
        int order;
    public:
        StructureField(string name,
                       CajetaType* type,
                       int arrayDimension,
                       bool reference,
                       Initializer* initializer,
                       set<Modifier> modifiers,
                       set<QualifiedName*> annotations,
                       int order) : Field(name, type, arrayDimension, reference, modifiers, annotations, initializer) {
            this->order = order;
        }
        void setOrder(int order) { this->order = order; }
        int getOrder() { return order; }
    };

} // cajeta