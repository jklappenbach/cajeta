//
// Created by James Klappenbach on 10/5/22.
//

#include "cajeta/ast/Block.h"
#include "cajeta/ast/BlockStatement.h"

namespace cajeta {
    Block* Block::fromContext(CajetaParser::BlockContext* ctxBlock, ParseContext* ctxParse,
                              llvm::Twine& name, llvm::Function* parent) {
        Block* block = new Block(ctxParse, name, parent);
        for (CajetaParser::BlockStatementContext* ctxBlockStatement : ctxBlock->blockStatement()) {
            BlockStatement* blockStatement = BlockStatement::fromContext(ctxBlockStatement);
            block->addBlockStatement(blockStatement);
        }
        return block;
    }

    llvm::Value* Block::codegen(ParseContext* ctxParse) {
        return nullptr;
    }
}
