//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/asn/Statement.h"
#include "cajeta/asn/Expression.h"

/**
 * statement
    : blockLabel=block
    | ASSERT expression (':' expression)? ';'
    | IF parExpression statement (ELSE statement)?
    | FOR '(' forControl ')' statement
    | WHILE parExpression statement
    | DO statement WHILE parExpression ';'
    | TRY block (catchClause+ finallyBlock? | finallyBlock)
    | TRY resourceSpecification block catchClause* finallyBlock?
    | SWITCH parExpression '{' switchBlockStatementGroup* switchLabel* '}'
    | SYNCHRONIZED parExpression block
    | RETURN expression? ';'
    | THROW expression ';'
    | BREAK identifier? ';'
    | CONTINUE identifier? ';'
    | YIELD expression ';' // Java17
    | SEMI
    | statementExpression=expression ';'
    | switchExpression ';'? // Java17
    | identifierLabel=identifier ':' statement
    ;

 * @param ctx
 */
namespace cajeta {
    Statement* Statement::fromContext(CajetaParser::StatementContext* ctx) {
        Statement* result;

        antlr4::Token* token = ctx->getStart();

        if (ctx->statementExpression) {
            cout << "Hit statementExpression in a Statement";
        }

        if (ctx->block()) {
            result = new LabelStatement(token);
        } else if (ctx->IF()) {
            result = new IfStatement(token);
        } else if (ctx->FOR()) {
            result = new ForStatement(token);
        } else if (ctx->WHILE()) {
            result = new WhileStatement(token);
        } else if (ctx->DO()) {
            result = new DoStatement(ctx);
        } else if (ctx->TRY()) {
            result = new TryStatement(token);
        } else if (ctx->SWITCH()) {
            result = new SwitchStatement(token);
        } else if (ctx->SYNCHRONIZED()) {
            result = new SynchronizedStatement(token);
        } else if (ctx->RETURN()) {
            result = new ReturnStatement(token);
        } else if (ctx->THROW()) {
            result = new ThrowStatement(token);
        } else if (ctx->BREAK()) {
            result = new BreakStatement(token);
        } else if (ctx->CONTINUE()) {
            result = new ContinueStatement(token);
        } else if (ctx->YIELD()) {
            result = new YieldStatement(token);
        } else if (ctx->expression()) {
            result = Expression::fromContext(ctx->expression());
        } else if (ctx->statementExpression) {
        } else if (ctx->switchExpression()) {
            cout << "Hit switch expression";
            //result = new SwitchExpression;
        } else if (ctx->identifierLabel) {
            result = new IdentifierLabel(ctx->getStart());
        } else if (ctx->SEMI()) {
            cout << "Hit SEMI statement";
        }

        return result;
    }

    llvm::Value* LabelStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* IfStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* ForStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* WhileStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* EnhancedForStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    DoStatement::DoStatement(CajetaParser::StatementContext* ctx) : Statement(ctx->getStart()) {
        for (auto statementContext: ctx->statement()) {
            statements.push_back(Statement::fromContext(ctx));
        }
        parExpression = new ParExpression(ctx->getStart());
    }

    llvm::Value* DoStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* TryStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* ResourceTryStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* SwitchStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* SynchronizedStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* ReturnStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* ThrowStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* BreakStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* ContinueStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* YieldStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* IdentifierLabel::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }

    llvm::Value* SemiStatement::generateCode(CajetaModule* compilationUnit) {
        return nullptr;
    }
}