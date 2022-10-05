//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "Type.h"
#include "Method.h"
#include "Field.h"

namespace cajeta {
    class Enum : public Type {
        Enum(QualifiedName* qName, set<Modifier>& modifiers) : Type(qName, modifiers) {
        }
    };
}