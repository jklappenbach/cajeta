//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/Statement.h"

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
Statement* Statement::create(CajetaParser::StatementContext* ctxStatement) {
    Statement* result;

    std::vector<CajetaParser::StatementContext*> statements = ctxStatement->statement();
    for (auto itr = statements.begin(); itr != statements.end(); itr++) {
        CajetaParser::StatementContext* statementContext = *itr;
        if (statementContext->block()) {
            result = new BlockLabelStatement;
        } else if (statementContext->ASSERT()) {
            result = new AssertStatement;
        } else if (statementContext->IF()) {
            result = new IfStatement;
        } else if (statementContext->FOR()) {
            result = new ForStatement;
        } else if (statementContext->WHILE()) {
            result = new WhileStatement;
        } else if (statementContext->DO()) {
            result = new DoStatement;
        } else if (statementContext->TRY()) {
            result = new TryStatement;
        } else if (statementContext->SWITCH()) {
            result = new SwitchStatement;
        } else if (statementContext->SYNCHRONIZED()) {
            result = new SynchronizedStatemment;
        } else if (statementContext->RETURN()) {
            result = new ReturnStatement;
        } else if (statementContext->THROW()) {
            result = new ThrowStatement;
        } else if (statementContext->BREAK()) {
            result = new BreakStatement;
        } else if (statementContext->CONTINUE()) {
            result = new ContinueStatement;
        } else if (statementContext->YIELD()) {
            result = new YieldStatement;
        } else if (statementContext->SEMI()) {
            cout << "Hit SEMI statement";
        } else if (statementContext->statementExpression) {
            cout << "Hit statementContext->statementExpression";
        } else if (statementContext->switchExpression()) {
            result = new SwitchExpression;
        } else if (statementContext->identifierLabel) {
            result = new IdentifierLabel;
        }
    }

    return result;
}
