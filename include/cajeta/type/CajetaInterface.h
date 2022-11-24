//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "cajeta/type/CajetaStructure.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/Field.h"

namespace cajeta {
    class CajetaInterface : public CajetaStructure {
        CajetaInterface(QualifiedName* qName)  { }
    };
}