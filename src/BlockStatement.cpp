//
// Created by James Klappenbach on 10/5/22.
//

#include "cajeta/ast/BlockStatement.h"
#include "cajeta/ast/LocalTypeDeclaration.h"
#include "cajeta/ast/LocalVariableDeclaration.h"
#include "cajeta/ast/Statement.h"


namespace cajeta {
    BlockStatement* BlockStatement::fromContext(CajetaParser::BlockStatementContext* ctx) {
        BlockStatement* result = nullptr;
        if (ctx->localTypeDeclaration()) {
            result = LocalTypeDeclaration::fromContext(ctx->localTypeDeclaration());
        } else if (ctx->localVariableDeclaration()) {

        } else if (ctx->statement()) {
            //result = Statement::fromContext(ctx->statement());
        } else if (ctx->SEMI()) {
            // TODO: figure out what to do here.
        }
    }

    llvm::Value* BlockStatement::codegen(ParseContext* ctxParse) {
        return nullptr;
    }
}