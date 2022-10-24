//
// Created by James Klappenbach on 10/5/22.
//

#pragma once

#include "cajeta/block/BlockStatement.h"
#include "cajeta/type/CajetaType.h"
#include <string>

using namespace std;

namespace cajeta {
    class LocalVariable : public BlockStatement {
        string name;
        CajetaType* type;
        bool var;
        bool reference;
    };
}
