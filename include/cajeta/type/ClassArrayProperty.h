//
// Created by James Klappenbach on 11/14/22.
//

#pragma once

#include <cajeta/type/ClassProperty.h>

namespace cajeta {
    class ClassArrayProperty : public ClassProperty {
    protected:
        int dimensionCount;
    public:
        ClassArrayProperty(string name,
            CajetaType* type,
            int order, int dimensionCount) : ClassProperty(name, type, order) {
            this->dimensionCount = dimensionCount;
        }

        ClassArrayProperty(string name,
            CajetaType* type,
            set<Modifier> modifiers,
            set<QualifiedName*> annotations,
            int order,
            int dimensionCount) : ClassProperty(name, type, modifiers, annotations, order) {
            this->dimensionCount = dimensionCount;
        }
    };
} // cajeta