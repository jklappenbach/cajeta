// DefaultConstructorMethod - the synthesized no-argument constructor.

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


