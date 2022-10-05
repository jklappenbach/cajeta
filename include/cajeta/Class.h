//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "Type.h"
#include "Field.h"
#include "Method.h"
#include "Modifiable.h"

namespace cajeta {
    class Class : public Type {
    public:
        Class(QualifiedName* qName, set<Modifier>& modifiers) : Type(qName, modifiers) {
        }
    };
}