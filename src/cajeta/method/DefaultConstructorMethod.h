//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include "Method.h"

using namespace std;

namespace cajeta {
    class CajetaModule;

    class Expression;

    class CajetaClass;

    class DefaultConstructorMethod : public Method {
    public:
        DefaultConstructorMethod(CajetaModulePtr module, CajetaClassPtr parent);

        void generateCode();
    };
}


