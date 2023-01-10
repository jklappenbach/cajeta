//
// Created by James Klappenbach on 10/5/22.
//

#pragma once

#include "llvm/IR/BasicBlock.h"
#include "BlockStatement.h"

#include "list"

using namespace std;

namespace cajeta {
    class Method;

    class BlockStatement;

    class Block : public AbstractSyntaxNode {
    public:
        Block(antlr4::Token* token) : AbstractSyntaxNode(token) { }

        Block() : AbstractSyntaxNode(nullptr) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    typedef shared_ptr<Block> BlockPtr;
}

