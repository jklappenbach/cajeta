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

    class CreateStructureMetadataMethod : public Method {
    public:
        CreateStructureMetadataMethod(CajetaStructure* parent);
        void generateCode(CajetaModule* compilationUnit) override;
    };
}


