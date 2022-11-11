//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaStructure.h"
#include "Field.h"
#include "Method.h"
#include "Modifiable.h"

namespace cajeta {
    class CajetaClass : public CajetaStructure {
    private:

    public:
        CajetaClass(CajetaModule* module, QualifiedName* qName)
                : CajetaStructure(module, qName) { }
    };
}