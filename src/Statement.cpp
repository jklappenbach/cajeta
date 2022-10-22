//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/ast/Statement.h"

using namespace cajeta;

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
//Statement* Statement::fromContext(CajetaParser::StatementContext* ctxStatement) {
//    Statement* result;
//
//    if (ctxStatement->statementExpression) {
//        cout << "Hit statementExpression in a Statement";
//    }
//
//    if (ctxStatement->block()) {
//        result = new BlockLabelStatement;
//    } else if (ctxStatement->ASSERT()) {
//        result = new AssertStatement;
//    } else if (ctxStatement->IF()) {
//        result = new IfStatement;
//    } else if (ctxStatement->FOR()) {
//        result = new ForStatement;
//    } else if (ctxStatement->WHILE()) {
//        result = new WhileStatement;
//    } else if (ctxStatement->DO()) {
//        result = new DoStatement;
//    } else if (ctxStatement->TRY()) {
//        result = new TryStatement;
//    } else if (ctxStatement->SWITCH()) {
//        result = new SwitchStatement;
//    } else if (ctxStatement->SYNCHRONIZED()) {
//        result = new SynchronizedStatemment;
//    } else if (ctxStatement->RETURN()) {
//        result = new ReturnStatement;
//    } else if (ctxStatement->THROW()) {
//        result = new ThrowStatement;
//    } else if (ctxStatement->BREAK()) {
//        result = new BreakStatement;
//    } else if (ctxStatement->CONTINUE()) {
//        result = new ContinueStatement;
//    } else if (ctxStatement->YIELD()) {
//        result = new YieldStatement;
//    } else if (ctxStatement->SEMI()) {
//        cout << "Hit SEMI statement";
//    } else if (ctxStatement->statementExpression) {
//        cout << "Hit ctxStatement->statementExpression";
//    } else if (ctxStatement->switchExpression()) {
//        result = new SwitchExpression;
//    } else if (ctxStatement->identifierLabel) {
//        result = new IdentifierLabel;
//    }
//
//    return result;
//}
