//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include "Method.h"

using namespace std;

namespace cajeta {
    class CajetaModule;

    class Expression;

    class CajetaStructure;

    class DefaultConstructorMethod : public Method {
    public:
        DefaultConstructorMethod(CajetaModulePtr module, CajetaStructurePtr parent);

        void generateCode();
    };
}


