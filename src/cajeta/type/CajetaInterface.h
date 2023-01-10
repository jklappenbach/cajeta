//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaStructure.h"
#include "../method/Method.h"
#include "../field/Field.h"

namespace cajeta {
    class CajetaInterface : public CajetaStructure {
        CajetaInterface(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qImplemented) : CajetaStructure(module, qName, qImplemented) { }
    };

    typedef shared_ptr<CajetaInterface> CajetaInterfacePtr;
}