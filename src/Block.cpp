//
// Created by James Klappenbach on 10/5/22.
//

#include "cajeta/ast/Block.h"
#include "cajeta/ast/BlockStatement.h"

namespace cajeta {
    Block* Block::fromContext(CajetaParser::BlockContext* ctxBlock, llvm::LLVMContext& llvmContext,
                              llvm::IRBuilder<>* builder, llvm::Twine& name, llvm::Function* parent) {
        Block* block = new Block(llvmContext, builder, name, parent);
        for (CajetaParser::BlockStatementContext* ctxBlockStatement : ctxBlock->blockStatement()) {
            BlockStatement* blockStatement = BlockStatement::fromContext(ctxBlockStatement);
            block->addBlockStatement(blockStatement);
        }
    }

    llvm::Value* Block::codegen(ParseContext* ctxParse) {
        return nullptr;
    }
}
