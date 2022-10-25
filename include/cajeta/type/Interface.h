//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "cajeta/type/CajetaType.h"
#include "Method.h"
#include "cajeta/type/Field.h"

namespace cajeta {
    class Interface : public CajetaType, public Modifiable, public Annotatable {
        Interface(QualifiedName* qName, set<Modifier>& modifiers) : Modifiable(modifiers), CajetaType(qName) { }
    };
}