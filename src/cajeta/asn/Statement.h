//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include "string"
#include "CajetaParser.h"
#include "../field/Field.h"
#include "BlockStatement.h"

using namespace std;

namespace cajeta {

    class Block;
    typedef shared_ptr<Block> BlockPtr;

    class Expression;
    typedef shared_ptr<Expression> ExpressionPtr;

    class ParExpression;

    class Statement;
    typedef shared_ptr<Statement> StatementPtr;

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

        static StatementPtr fromContext(CajetaParser::StatementContext* ctx);
    };

    class ExpressionStatement : public Statement {
    private:
        ExpressionPtr expression;
    public:
        ExpressionStatement(ExpressionPtr expression, antlr4::Token* token) : Statement(token) {
            this->expression = expression;
        }

        llvm::Value* generateCode(CajetaModulePtr module) override {
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

        llvm::Value* generateCode(CajetaModulePtr module) override;
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

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * FOR '(' forControl ')' statement
     */
    class ForStatement : public Statement {
    private:
        list<FieldPtr> initializer;
        Expression* control;
        list<Expression*> update;
    public:
        ForStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * variableModifier* (typeType | VAR) variableDeclaratorId ':' expression
     */
    class EnhancedForStatement : public Statement {
    private:
        list<FieldPtr> fields;
        bool var;
        string variableName;
        Expression* expression;
    public:
        EnhancedForStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * WHILE parExpression statement
     */
    class WhileStatement : public Statement {
    private:
        ExpressionPtr parExpression;
        StatementPtr statement;
    public:
        WhileStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * DO statement WHILE parExpression ';'
     */
    class DoStatement : public Statement {
    private:
        list<StatementPtr> statements;
        ExpressionPtr parExpression;
    public:
        DoStatement(CajetaParser::StatementContext* ctx);

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class CatchClause {
    private:
        list<FieldPtr> catchFields;
        Block* catchBlock;
    public:

    };

    /**
     * TRY block (catchClause+ finallyBlock? | finallyBlock)
     */
    class TryStatement : public Statement {
    private:
        BlockPtr block;
        list<CatchClause> catchClauses;
        BlockPtr finally;
    public:
        TryStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * TRY resourceSpecification block catchClause* finallyBlock?
     */
    class ResourceTryStatement : public Statement {
    private:
        list<FieldPtr> resources;
        BlockPtr block;
        list<CatchClause> catchClauses;
        BlockPtr finally;
    public:
        ResourceTryStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class SwitchBlockStatement {

    };
    typedef shared_ptr<SwitchBlockStatement> SwitchBlockStatementPtr;

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
        ExpressionPtr parExpression;
        list<SwitchBlockStatementPtr> switchBlockStatements;
    public:
        SwitchStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * SYNCHRONIZED parExpression block
     */
    class SynchronizedStatement : public Statement {
    private:
        ExpressionPtr parExpression;
        BlockPtr block;
    public:
        SynchronizedStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * RETURN expression? ';'
     */
    class ReturnStatement : public Statement {
    private:
        ExpressionPtr expression;
    public:
        ReturnStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * THROW expression ';'
     */
    class ThrowStatement : public Statement {
    private:
        ExpressionPtr expression;
    public:
        ThrowStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * BREAK identifier? ';'
     */
    class BreakStatement : public Statement {
    private:
        string identifier;
    public:
        BreakStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * CONTINUE identifier? ';'
     */
    class ContinueStatement : public Statement {
    private:
        string identifier;
    public:
        ContinueStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * YIELD expression ';' // Java17
     */
    class YieldStatement : public Statement {
    private:
    public:
        YieldStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
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

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class SemiStatement : public Statement {
    public:
        SemiStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };
}