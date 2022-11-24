//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaStructure.h"
#include "Field.h"
#include "cajeta/method/Method.h"
#include "Modifiable.h"

namespace cajeta {
    class CajetaClass : public CajetaStructure {
    private:

    public:
        CajetaClass(QualifiedName* qName)
                : CajetaStructure(qName) { }
        virtual int getStructType() { return STRUCT_TYPE_CLASS; }
    };

}