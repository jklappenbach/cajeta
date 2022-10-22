//
// Created by James Klappenbach on 10/5/22.
//

#pragma once

#include "cajeta/ast/Statement.h"
#include "LocalVariable.h"

#include <list>

using namespace std;

namespace cajeta {
    class Block {
        list<BlockStatement*> blockStatements;
    };
}

