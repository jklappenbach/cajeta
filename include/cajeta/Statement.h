//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <string>
#include "CajetaParser.h"

using namespace std;

namespace cajeta {
    struct Expression {

    };

/**
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

 */
    struct Statement {
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

         * @param ctxStatement
         */
        static Statement* create(CajetaParser::StatementContext* ctxStatement);
    };

    struct BlockLabelStatement : Statement {
        string labelName;
    };

    struct AssertStatement : Statement {
        Expression expression;
    };

    struct AssignmentStatement : Statement {

    };

    struct IfStatement : Statement {

    };

    struct ForStatement : Statement {

    };

    struct WhileStatement : Statement {

    };

    struct DoStatement : Statement {

    };

    struct TryStatement : Statement {

    };

    struct SwitchStatement : Statement {

    };

    struct SynchronizedStatemment : Statement {

    };

    struct ReturnStatement : Statement {

    };

    struct ThrowStatement : Statement {

    };

    struct BreakStatement : Statement {

    };

    struct ContinueStatement : Statement {

    };

    struct YieldStatement : Statement {

    };

    struct SwitchExpression : Statement {

    };

    struct IdentifierLabel : Statement {

    };

    /**
     *             result = new BreakStatement;
        } else if (statementContext->CONTINUE()) {
            result = new ContinueStatement;
        } else if (statementContext->YIELD()) {
            result = new YieldStatement;
        } else if (statementContext->SEMI()) {
            result = new SemiStatement;
        } else if (statementContext->statementExpression) {
            result = new StatementExpression;
        } else if (statementContext->switchExpression()) {
            result = new SwitchExpression;
        } else if (statementContext->identifierLabel) {
            result = new IdentifierLabel;
     */
}