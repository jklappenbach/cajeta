//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include "cajeta/method/Method.h"

using namespace std;

namespace cajeta {
    class CajetaModule;
    class Expression;
    class CajetaStructure;

    class DefaultDestructorMethod : public Method {
    public:
        DefaultDestructorMethod(CajetaStructure* parent);
        void generateCode(CajetaModule* compilationUnit);
    };
}


