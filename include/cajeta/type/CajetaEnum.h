//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "cajeta/type/CajetaType.h"
#include "Method.h"
#include "Field.h"

namespace cajeta {
    class CajetaEnum : public CajetaType {
        CajetaEnum(QualifiedName* qName, set<Modifier>& modifiers) : CajetaType(qName, modifiers) {
        }
    };
}