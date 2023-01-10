//
// Created by James Klappenbach on 2/19/22.
//

#include "Statement.h"
#include "expression/Expression.h"
#include "../compile/CajetaModule.h"

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
    StatementPtr Statement::fromContext(CajetaParser::StatementContext* ctx) {
        StatementPtr result;

        antlr4::Token* token = ctx->getStart();

        if (ctx->statementExpression) {
            cout << "Hit statementExpression in a Statement";
        }

        if (ctx->block()) {
            result = make_shared<LabelStatement>(token);
        } else if (ctx->IF()) {
            result = make_shared<IfStatement>(token);
        } else if (ctx->FOR()) {
            result = make_shared<ForStatement>(token);
        } else if (ctx->WHILE()) {
            result = make_shared<WhileStatement>(token);
        } else if (ctx->DO()) {
            result = make_shared<DoStatement>(ctx);
        } else if (ctx->TRY()) {
            result = make_shared<TryStatement>(token);
        } else if (ctx->SWITCH()) {
            result = make_shared<SwitchStatement>(token);
        } else if (ctx->SYNCHRONIZED()) {
            result = make_shared<SynchronizedStatement>(token);
        } else if (ctx->RETURN()) {
            result = make_shared<ReturnStatement>(token);
        } else if (ctx->THROW()) {
            result = make_shared<ThrowStatement>(token);
        } else if (ctx->BREAK()) {
            result = make_shared<BreakStatement>(token);
        } else if (ctx->CONTINUE()) {
            result = make_shared<ContinueStatement>(token);
        } else if (ctx->YIELD()) {
            result = make_shared<YieldStatement>(token);
        } else if (ctx->expression()) {
            result = Expression::fromContext(ctx->expression());
        } else if (ctx->statementExpression) {
        } else if (ctx->switchExpression()) {
            cout << "Hit switch expression";
            //result = new SwitchExpression;
        } else if (ctx->identifierLabel) {
            result = make_shared<IdentifierLabel>(ctx->getStart());
        } else if (ctx->SEMI()) {
            cout << "Hit SEMI statement";
        }

        return result;
    }

    llvm::Value* LabelStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* IfStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* ForStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* WhileStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* EnhancedForStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    DoStatement::DoStatement(CajetaParser::StatementContext* ctx) : Statement(ctx->getStart()) {
        for (auto statementContext: ctx->statement()) {
            statements.push_back(Statement::fromContext(ctx));
        }
        parExpression = make_shared<ParExpression>(ctx->getStart());
    }

    llvm::Value* DoStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* TryStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* ResourceTryStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* SwitchStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* SynchronizedStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* ReturnStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* ThrowStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* BreakStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* ContinueStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* YieldStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* IdentifierLabel::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* SemiStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }
}