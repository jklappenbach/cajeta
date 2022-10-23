//
// Created by James Klappenbach on 10/5/22.
//

#pragma once

#include "cajeta/ast/AbstractSyntaxTree.h"
#include "cajeta/ast/Statement.h"

namespace cajeta {
    class BlockStatement : public AbstractSyntaxTree {
    private:
        Statement* statement;
    public:
        static BlockStatement* fromContext(CajetaParser::BlockStatementContext* ctx);
    };
}