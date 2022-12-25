//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <string>
#include "CajetaParser.h"
#include "cajeta/field/Field.h"
#include "cajeta/asn/BlockStatement.h"

using namespace std;

namespace cajeta {

    class Block;

    class Expression;

    class ParExpression;

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
    public:
        Statement(antlr4::Token* token) : BlockStatement(token) { }

        static Statement* fromContext(CajetaParser::StatementContext* ctx);
    };

    class ExpressionStatement : public Statement {
    private:
        Expression* expression;
    public:
        ExpressionStatement(Expression* expression, antlr4::Token* token) : Statement(token) {
            this->expression = expression;
        }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override {
            return nullptr;
        }
    };

    /**
     * blockLabel=block
     */
    class LabelStatement : public Statement {
    private:
        string labelName;
        Block* block;
    public:
        LabelStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    class AssignmentStatement : public Statement {

    };

    /**
     * IF parExpression statement (ELSE statement)?
     */
    class IfStatement : public Statement {
    private:
        ParExpression* parExpression;
        Statement* statement;
        Statement* elseClause;
    public:
        IfStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * FOR '(' forControl ')' statement
     */
    class ForStatement : public Statement {
    private:
        list<Field*> initializer;
        Expression* control;
        list<Expression*> update;
    public:
        ForStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * variableModifier* (typeType | VAR) variableDeclaratorId ':' expression
     */
    class EnhancedForStatement : public Statement {
    private:
        list<Field*> fields;
        bool var;
        string variableName;
        Expression* expression;
    public:
        EnhancedForStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * WHILE parExpression statement
     */
    class WhileStatement : public Statement {
    private:
        ParExpression* parExpression;
        Statement* statement;
    public:
        WhileStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * DO statement WHILE parExpression ';'
     */
    class DoStatement : public Statement {
    private:
        list<Statement*> statements;
        ParExpression* parExpression;
    public:
        DoStatement(CajetaParser::StatementContext* ctx);

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    class CatchClause {
    private:
        list<Field*> catchFields;
        Block* catchBlock;
    public:

    };

    /**
     * TRY block (catchClause+ finallyBlock? | finallyBlock)
     */
    class TryStatement : public Statement {
    private:
        Block* block;
        list<CatchClause*> catchClauses;
        Block* finally;
    public:
        TryStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * TRY resourceSpecification block catchClause* finallyBlock?
     */
    class ResourceTryStatement : public Statement {
    private:
        list<Field*> resources;
        Block* block;
        list<CatchClause*> catchClauses;
        Block* finally;
    public:
        ResourceTryStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
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
    private:
        Expression* parExpression;
        list<SwitchBlockStatement*> switchBlockStatements;
    public:
        SwitchStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * SYNCHRONIZED parExpression block
     */
    class SynchronizedStatement : public Statement {
    private:
        Expression* parExpression;
        Block* block;
    public:
        SynchronizedStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * RETURN expression? ';'
     */
    class ReturnStatement : public Statement {
    private:
        Expression* expression;
    public:
        ReturnStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * THROW expression ';'
     */
    class ThrowStatement : public Statement {
    private:
        Expression* expression;
    public:
        ThrowStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * BREAK identifier? ';'
     */
    class BreakStatement : public Statement {
    private:
        string identifier;
    public:
        BreakStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * CONTINUE identifier? ';'
     */
    class ContinueStatement : public Statement {
    private:
        string identifier;
    public:
        ContinueStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    /**
     * YIELD expression ';' // Java17
     */
    class YieldStatement : public Statement {
    private:
    public:
        YieldStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
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
    private:
        string identifier;
    public:
        IdentifierLabel(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

    class SemiStatement : public Statement {
    public:
        SemiStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };
}