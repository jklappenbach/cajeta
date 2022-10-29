//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <string>
#include "BlockStatement.h"
#include "CajetaParser.h"
#include "cajeta/type/Field.h"
#include "cajeta/ast/Expression.h"
#include "cajeta/ast/AbstractSyntaxTree.h"

using namespace std;

namespace cajeta {

    class Block;

//    statement
//    : blockLabel=block
//    | IF parExpression statement (ELSE statement)?
//    | FOR '(' forControl ')' statement
//    | WHILE parExpression statement
//    | DO statement WHILE parExpression ';'
//    | TRY block (catchClause+ finallyBlock? | finallyBlock)
//    | TRY resourceSpecification block catchClause* finallyBlock?
//    | SWITCH parExpression '{' switchBlockStatementGroup* switchLabel* '}'
//    | SYNCHRONIZED parExpression block
//    | RETURN expression? ';'
//    | THROW expression ';'
//    | BREAK identifier? ';'
//    | CONTINUE identifier? ';'
//    | YIELD expression ';' // Java17
//    | SEMI
//    | statementExpression=expression ';'
//    | switchExpression ';'? // Java17
//    | identifierLabel=identifier ':' statement
//    ;

    class Statement : public BlockStatement {
        static Statement* fromContext(CajetaParser::StatementContext* ctxStatement);
    };


    /**
     * blockLabel=block
     */
    class BlockLabelStatement : public Statement {
        string labelName;
        Block* block;
    };

    class AssignmentStatement : public Statement {

    };

    /**
     * IF parExpression statement (ELSE statement)?
     */
    class IfStatement : public Statement {
        Expression* parExpression;
        Statement* statement;
        Statement* elseClause;
    };

    /**
     * FOR '(' forControl ')' statement
     */
    class ForStatement : public Statement {
        list<Field*> initializer;
        Expression* control;
        list<Expression*> update;
    };

    /**
     * variableModifier* (typeType | VAR) variableDeclaratorId ':' expression
     */
    class EnhancedForStatement : public Statement {
        list<Field*> fields;
        bool var;
        string variableName;
        Expression* expression;
    };

    /**
     * WHILE parExpression statement
     */
    class WhileStatement : public Statement {
        Expression* parExpression;
        Statement* statement;
    };

    /**
     * DO statement WHILE parExpression ';'
     */
    class DoStatement : public Statement {
        Statement* statement;
        Expression* parExpression;
    };

    class CatchClause {
        list<Field*> catchFields;
        Block* catchBlock;
    };

    /**
     * TRY block (catchClause+ finallyBlock? | finallyBlock)
     */
    class TryStatement : public Statement {
        Block* block;
        list<CatchClause*> catchClauses;
        Block* finally;
    };

    /**
     * TRY resourceSpecification block catchClause* finallyBlock?
     */
    class ResourceTryStatement : public Statement {
        list<Field*> resources;
        Block* block;
        list<CatchClause*> catchClauses;
        Block* finally;
    };

    class SwitchBlockStatement {

    };

    class SwitchBlockStatementGroup : public SwitchBlockStatement {
        string switchLabel;
        Block* switchBlock;
    };

    class SwitchLabel : public SwitchBlockStatement {
        string switchLabel;
    };

    /**
     * SWITCH parExpression '{' switchBlockStatementGroup* switchLabel* '}'
     */
    class SwitchStatement : public Statement {
        Expression* parExpression;
        list<SwitchBlockStatement*> switchBlockStatements;
    };

    /**
     * SYNCHRONIZED parExpression block
     */
    class SynchronizedStatement : public Statement {
        Expression* parExpression;
        Block* block;
    };

    /**
     * RETURN expression? ';'
     */
    class ReturnStatement : public Statement {
        Expression* expression;
    };

    /**
     * THROW expression ';'
     */
    class ThrowStatement : public Statement {
        Expression* expression;
    };

    /**
     * BREAK identifier? ';'
     */
    class BreakStatement : public Statement {
        string identifier;
    };

    /**
     * CONTINUE identifier? ';'
     */
    class ContinueStatement : public Statement {
        string identifier;
    };

    /**
     * YIELD expression ';' // Java17
     */
    class YieldStatement : public Statement {

    };

//    TODO: Java17 switch statement
//    /**
//     * guardedPattern
//     * : '(' guardedPattern ')'
//     * | variableModifier* typeType annotation* identifier ('&&' expression)*
//     * | guardedPattern '&&' expression;
//     */
//    class GuardedPattern {
//        GuardedPattern* guardedPattern;
//
//    };
//
//    /**
//     * switchLabeledRule
//     * : CASE (expressionList | NULL_LITERAL | guardedPattern) (ARROW | COLON) switchRuleOutcome
//     * | DEFAULT (ARROW | COLON) switchRuleOutcome;
//     */
//    class SwitchLabelRule {
//       list<Expression*>
//    };
//    /**
//     * switchExpression ';'? // Java17
//     */
//    class SwitchExpression : public Statement {
//        Expression* parExpression;
//    };

    class IdentifierLabel : public Statement {
        string identifier;
    };

    class SemiStatement : public Statement {
    };
}