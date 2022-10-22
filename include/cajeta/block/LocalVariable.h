//
// Created by James Klappenbach on 10/5/22.
//

#pragma once

#include "cajeta/block/BlockStatement.h"
#include "cajeta/Type.h"
#include <string>

using namespace std;

namespace cajeta {
    class LocalVariable : public BlockStatement {
        string name;
        Type* type;
        bool var;
        bool reference;
    };
}
