//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaStructure.h"
#include "Field.h"
#include "cajeta/method/Method.h"
#include "Modifiable.h"

namespace cajeta {
    class CajetaModule;

    class CajetaClass : public CajetaStructure {
    private:

    public:
        CajetaClass(CajetaModule* module, QualifiedName* qName)
            : CajetaStructure(module, qName) { }

        virtual int getStructType() { return STRUCT_TYPE_CLASS; }
    };

}