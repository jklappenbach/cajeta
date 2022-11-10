//
// Created by James Klappenbach on 10/5/22.
//

#pragma once

#include "AbstractSyntaxNode.h"
#include <CajetaParser.h>

namespace cajeta {
    class Statement;

    class BlockStatement : public AbstractSyntaxNode {
    public:
        BlockStatement(antlr4::Token* token) : AbstractSyntaxNode(token) { }
    };

    class DefaultBlockStatement : public BlockStatement {
    public:
        llvm::Value* generateCode(CajetaModule* module) override;
    };
}
