//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <string>

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

    };

    struct BlockLabel : Statement {
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
}