//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaStructure.h"
#include "Field.h"
#include "Method.h"
#include "Modifiable.h"
#include <stdio.h>

namespace cajeta {
    class CajetaArray : public CajetaStructure {
    private:
        int dimension;
        CajetaType* elementType;
    public:
        CajetaArray(CajetaType* elementType, int dimension) {
            char buffer[256];
            snprintf(buffer, 255, "%s[%d]", elementType->getQName()->toCanonical().c_str(), dimension);
            qName = QualifiedName::getOrInsert(string(buffer));
        }
        virtual void generateSignature(CajetaModule* module);
        virtual void generateCode(CajetaModule* module);
    };
}