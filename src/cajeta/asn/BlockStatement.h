// BlockStatement - base for statements inside a block, plus the default wrapper.

#pragma once

#include "AbstractSyntaxNode.h"
#include "CajetaParser.h"

namespace cajeta {
    class Statement;

    class BlockStatement : public AbstractSyntaxNode {
    public:
        BlockStatement(antlr4::Token* token) : AbstractSyntaxNode(token) { }
    };
    typedef shared_ptr<BlockStatement> BlockStatementPtr;

    class DefaultBlockStatement : public BlockStatement {
    public:
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };
}
