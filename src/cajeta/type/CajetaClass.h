//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaStructure.h"
#include "../field/Field.h"
#include "../method/Method.h"
#include "Modifiable.h"

namespace cajeta {
    class CajetaModule;

    class CajetaClass : public CajetaStructure {
    private:

    public:
        CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qExtended, list<QualifiedNamePtr> qImplemented)
            : CajetaStructure(module, qName, qExtended, qImplemented) { }

        virtual CajetaTypeFlags getTypeFlags() { return USER_DEFINED_FLAG; }
    };

}