//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaClass.h"
#include "../method/Method.h"
#include "../field/Field.h"

namespace cajeta {
    class CajetaInterface : public CajetaClass {
        CajetaInterface(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qImplemented) : CajetaClass(module, qName, qImplemented) { }
    };

    typedef shared_ptr<CajetaInterface> CajetaInterfacePtr;
}