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

    class Statement;
    typedef shared_ptr<Statement> StatementPtr;

//    statement
//    : blockLabel=block
//    | IF parExpression statement (ELSE statement)?
//    | FOR '(' forControl ')' statement
//    | WHILE parExpression statement
//    | DO statement WHILE parExpression ';'
//    | TRY block (catchClause+ finallyBlock? | finallyBlock)
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

        // Public alias to the file-internal block builder so callers outside
        // Statement.cpp (e.g. LambdaExpression's parser branch, which needs
        // a Block to wrap a block-form lambda body) can construct a Block
        // from its parser context without duplicating BlockStatement
        // assembly logic. Delegates to the same private helper used inside
        // Statement.cpp.
        static BlockPtr buildBlockFromContext(CajetaParser::BlockContext* ctx);
    };

    class ExpressionStatement : public Statement {
    private:
        ExpressionPtr expression;
    public:
        ExpressionStatement(ExpressionPtr expression, antlr4::Token* token) : Statement(token) {
            this->expression = expression;
        }

        ExpressionPtr getExpression() const { return expression; }

        // The wrapped expression isn't in `children` so the default walk skips it. Forward
        // explicitly so the type-resolver pre-pass visits every Expression in the tree.
        // Body in Statement.cpp because Expression is forward-declared here.
        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * blockLabel=block
     *
     * A block used in statement position (e.g. the body of `if (x) { ... }`). Wraps
     * a Block of inner block-statements. The "label" naming is historical — labels
     * proper (`outer: while (...) ...`) aren't implemented yet.
     */
    class LabelStatement : public Statement {
    private:
        BlockPtr block;
    public:
        LabelStatement(antlr4::Token* token, BlockPtr block)
            : Statement(token), block(block) { }

        // Block isn't in `children`, so walkers that visit only the
        // children list (lambda-body free-name collection, transfer-
        // name collection, value-capture immutability) need this
        // accessor to descend into nested-block contents.
        BlockPtr getBlock() const { return block; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // cajeta-docs/stdlib/Thread.md — `scope { ... }` is a structured-concurrency block that
    // owns every Task spawned inside it; control doesn't leave the block
    // until every child task has finished or been cancelled. In the sync-
    // lowering MVP this is just a block: spawns run inline, so there are no
    // outstanding children at the closing `}`. The class exists now so later
    // phases (real scheduler, scope-bound joins) have an AST hook without
    // having to re-parse.
    class ScopeStatement : public Statement {
    private:
        BlockPtr block;
    public:
        ScopeStatement(antlr4::Token* token, BlockPtr block)
            : Statement(token), block(block) { }

        BlockPtr getBlock() const { return block; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class AssignmentStatement : public Statement {

    };

    /**
     * IF parExpression statement (ELSE statement)?
     */
    class IfStatement : public Statement {
    private:
        ExpressionPtr condition;
        StatementPtr thenBranch;
        StatementPtr elseBranch;
    public:
        IfStatement(antlr4::Token* token, ExpressionPtr cond, StatementPtr thenStmt,
                    StatementPtr elseStmt)
            : Statement(token), condition(cond), thenBranch(thenStmt), elseBranch(elseStmt) { }

        // Exposed so external walkers (e.g. the lambda body's free-variable
        // scan) can reach sub-expressions hidden behind private fields.
        ExpressionPtr getCondition() const { return condition; }
        StatementPtr getThenBranch() const { return thenBranch; }
        StatementPtr getElseBranch() const { return elseBranch; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * FOR '(' forControl ')' statement
     *
     * C-style: `for (init; cond; update) body`. init may be a local-variable
     * declaration or a list of expression statements; cond and update are optional.
     */
    class ForStatement : public Statement {
    private:
        BlockStatementPtr init;       // LocalVariableDeclaration or ExpressionStatement; may be null
        ExpressionPtr condition;       // optional — null means "always true"
        list<ExpressionPtr> update;    // run after each iteration
        StatementPtr body;
    public:
        ForStatement(antlr4::Token* token, BlockStatementPtr init, ExpressionPtr cond,
                     list<ExpressionPtr> update, StatementPtr body)
            : Statement(token), init(init), condition(cond),
              update(std::move(update)), body(body) { }

        BlockStatementPtr getInit() const { return init; }
        ExpressionPtr getCondition() const { return condition; }
        const list<ExpressionPtr>& getUpdate() const { return update; }
        StatementPtr getBody() const { return body; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * Enhanced for-loop over an iterable (today: arrays only).
     *
     *   for ([iteratorType iteratorName ,] elementType elementName : iterable)
     *       body
     *
     * The optional iterator binding is a Cajeta extension that exposes the running
     * 0-based index alongside the element value.
     */
    class EnhancedForStatement : public Statement {
    private:
        CajetaTypePtr iteratorType;     // null if no iterator binding
        string iteratorName;
        CajetaTypePtr elementType;
        string elementName;
        ExpressionPtr iterableExpr;
        StatementPtr body;
    public:
        EnhancedForStatement(antlr4::Token* token,
                              CajetaTypePtr iteratorType,
                              string iteratorName,
                              CajetaTypePtr elementType,
                              string elementName,
                              ExpressionPtr iterableExpr,
                              StatementPtr body)
            : Statement(token),
              iteratorType(iteratorType),
              iteratorName(std::move(iteratorName)),
              elementType(elementType),
              elementName(std::move(elementName)),
              iterableExpr(iterableExpr),
              body(body) { }

        ExpressionPtr getIterableExpr() const { return iterableExpr; }
        StatementPtr getBody() const { return body; }
        CajetaTypePtr getElementType() const { return elementType; }
        const string& getElementName() const { return elementName; }
        CajetaTypePtr getIteratorType() const { return iteratorType; }
        const string& getIteratorName() const { return iteratorName; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * WHILE parExpression statement
     */
    class WhileStatement : public Statement {
    private:
        ExpressionPtr condition;
        StatementPtr body;
    public:
        WhileStatement(antlr4::Token* token, ExpressionPtr cond, StatementPtr body)
            : Statement(token), condition(cond), body(body) { }

        ExpressionPtr getCondition() const { return condition; }
        StatementPtr getBody() const { return body; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * DO statement WHILE parExpression ';'
     */
    class DoStatement : public Statement {
    private:
        StatementPtr body;
        ExpressionPtr condition;
    public:
        DoStatement(antlr4::Token* token, StatementPtr body, ExpressionPtr cond)
            : Statement(token), body(body), condition(cond) { }

        StatementPtr getBody() const { return body; }
        ExpressionPtr getCondition() const { return condition; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // One catch clause within a try. Currently single-type per catch (no `T1 | T2`
    // multi-catch), and the bound variable is implicitly the thrown value loaded via
    // the runtime accessor.
    struct CatchClause {
        CajetaTypePtr type;       // exception type (primitives + classes for now)
        string variableName;       // bound name in the catch body
        BlockPtr body;
    };

    /**
     * TRY block (catchClause+ finallyBlock? | finallyBlock)
     *
     * setjmp/longjmp-based exception handling: each try-block allocates a frame on
     * the stack, registers it with the runtime, and uses setjmp to set a recovery
     * point. `throw` longjmps back to the most recently registered frame.
     */
    class TryStatement : public Statement {
    private:
        BlockPtr tryBlock;
        std::vector<CatchClause> catchClauses;
        BlockPtr finallyBlock;
    public:
        TryStatement(antlr4::Token* token, BlockPtr tryBlock,
                     std::vector<CatchClause> catches, BlockPtr finallyBlock)
            : Statement(token), tryBlock(tryBlock),
              catchClauses(std::move(catches)), finallyBlock(finallyBlock) { }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // ResourceTryStatement was removed 2026-05-20 together with the
    // grammar's `TRY resourceSpecification …` alternative —
    // destructors fire deterministically at scope exit and made
    // try-with-resources redundant. See cajeta-docs/MemoryModel.md
    // § Destructors.

    // One labeled group within a switch — e.g. `case 1: case 2: stmts...`.
    // `caseValues` are the constant expressions for the case labels; an empty list
    // with `isDefault=true` means the `default:` group.
    struct SwitchGroup {
        std::vector<ExpressionPtr> caseValues;
        bool isDefault = false;
        std::vector<BlockStatementPtr> statements;
    };

    /**
     * SWITCH parExpression '{' switchBlockStatementGroup* switchLabel* '}'
     *
     * Classic Java switch over an integer subject. Groups fall through to the next
     * one unless terminated by `break` or `return`. `default:` matches when no case
     * does. The new-style `case X -> body` form is out of scope this round.
     */
    class SwitchStatement : public Statement {
    private:
        ExpressionPtr subject;
        std::vector<SwitchGroup> groups;
    public:
        SwitchStatement(antlr4::Token* token, ExpressionPtr subject,
                        std::vector<SwitchGroup> groups)
            : Statement(token), subject(subject), groups(std::move(groups)) { }

        void resolveTypes(CajetaModulePtr module) override;
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
        ReturnStatement(antlr4::Token* token, ExpressionPtr expression = nullptr)
            : Statement(token), expression(expression) { }

        // Like ExpressionStatement, the returned expression isn't in `children`.
        void resolveTypes(CajetaModulePtr module) override;

        // Exposed so external walkers (e.g. the lambda body's free-variable
        // scan) can reach the returned expression that isn't in `children`.
        ExpressionPtr getExpression() const { return expression; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * THROW expression ';'
     */
    class ThrowStatement : public Statement {
    private:
        ExpressionPtr expression;
    public:
        ThrowStatement(antlr4::Token* token, ExpressionPtr expression = nullptr)
            : Statement(token), expression(expression) { }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * BREAK identifier? ';'
     */
    class BreakStatement : public Statement {
    private:
        string label;   // empty for unlabeled `break;`
    public:
        BreakStatement(antlr4::Token* token) : Statement(token) { }
        BreakStatement(antlr4::Token* token, string label)
            : Statement(token), label(std::move(label)) { }
        const string& getLabel() const { return label; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * CONTINUE identifier? ';'
     */
    class ContinueStatement : public Statement {
    private:
        string label;   // empty for unlabeled `continue;`
    public:
        ContinueStatement(antlr4::Token* token) : Statement(token) { }
        ContinueStatement(antlr4::Token* token, string label)
            : Statement(token), label(std::move(label)) { }
        const string& getLabel() const { return label; }

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
        StatementPtr body;
    public:
        IdentifierLabel(antlr4::Token* token) : Statement(token) { }
        IdentifierLabel(antlr4::Token* token, string label, StatementPtr body)
            : Statement(token), identifier(std::move(label)), body(std::move(body)) { }

        const string& getIdentifier() const { return identifier; }
        StatementPtr getBody() const { return body; }

        void resolveTypes(CajetaModulePtr module) override {
            if (body) body->resolveTypes(module);
        }
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class SemiStatement : public Statement {
    public:
        SemiStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };
}