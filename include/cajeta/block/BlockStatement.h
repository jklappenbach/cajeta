//
// Created by James Klappenbach on 10/5/22.
//

#pragma once

#include "cajeta/ast/AbstractSyntaxTree.h"
#include "CajetaParser.h"


namespace cajeta {
    class Statement;

    class BlockStatement : public AbstractSyntaxTree {
    private:
        Statement* statement;
    public:
        static BlockStatement* fromContext(CajetaParser::BlockStatementContext* ctx);
    };
}
