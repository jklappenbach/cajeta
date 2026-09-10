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

    // The statement AST: one class per alternative of the grammar's `statement`
    // rule, each carrying the production it was built from.

    class Statement : public BlockStatement {
    public:
        Statement(antlr4::Token* token) : BlockStatement(token) { }

        static StatementPtr fromContext(CajetaParser::StatementContext* ctx);

        // The file-internal block builder, exposed for callers outside
        // Statement.cpp (LambdaExpression wraps a block-form body in a Block).
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

        // The wrapped expression is not in `children`, so the default walk skips
        // it. Body in the .cpp, since Expression is only forward-declared here.
        void resolveTypes(CajetaModulePtr module) override;
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** blockLabel=block — a block in statement position, e.g. the body of
     *  `if (x) { ... }`. The "label" naming is historical: labels are not built. */
    class LabelStatement : public Statement {
    private:
        BlockPtr block;
    public:
        LabelStatement(antlr4::Token* token, BlockPtr block)
            : Statement(token), block(block) { }

        // Block is not in `children`, so walkers that visit only that list need
        // this accessor to descend into the nested block.
        BlockPtr getBlock() const { return block; }

        void resolveTypes(CajetaModulePtr module) override;
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // `scope { ... }` — a structured-concurrency block owning every Task spawned
    // inside it (Concurrency.md). Spawns run inline under the sync-lowering MVP,
    // so this is just a block; the class exists as the hook for later phases.
    class ScopeStatement : public Statement {
    private:
        BlockPtr block;
    public:
        ScopeStatement(antlr4::Token* token, BlockPtr block)
            : Statement(token), block(block) { }

        BlockPtr getBlock() const { return block; }

        void resolveTypes(CajetaModulePtr module) override;
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class AssignmentStatement : public Statement {

    };

    /** IF parExpression statement (ELSE statement)? */
    class IfStatement : public Statement {
    private:
        ExpressionPtr condition;
        StatementPtr thenBranch;
        StatementPtr elseBranch;
    public:
        IfStatement(antlr4::Token* token, ExpressionPtr cond, StatementPtr thenStmt,
                    StatementPtr elseStmt)
            : Statement(token), condition(cond), thenBranch(thenStmt), elseBranch(elseStmt) { }

        // Exposed for external walkers, e.g. the lambda free-variable scan.
        ExpressionPtr getCondition() const { return condition; }
        StatementPtr getThenBranch() const { return thenBranch; }
        StatementPtr getElseBranch() const { return elseBranch; }

        void resolveTypes(CajetaModulePtr module) override;
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** FOR '(' forControl ')' statement — C-style `for (init; cond; update) body`.
     *  init may be a local declaration or expression statements; cond and update
     *  are both optional. */
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
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** Enhanced for over an iterable (today: arrays only):
     *  `for ([iteratorType iter,] elementType elem : iterable) body`. The optional
     *  iterator binding is a Cajeta extension exposing the running 0-based index. */
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
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** WHILE parExpression statement */
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
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** DO statement WHILE parExpression ';' */
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
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // One catch clause of a try: single-type per catch (no `T1 | T2`), with the
    // bound variable loaded through the runtime accessor.
    struct CatchClause {
        CajetaTypePtr type;       // exception type (primitives + classes for now)
        string typeNameText;      // the type name AS WRITTEN — re-resolved
                                  // scoped at resolveTypes
        string variableName;       // bound name in the catch body
        BlockPtr body;
    };

    /** TRY block (catchClause+ finallyBlock? | finallyBlock) — setjmp/longjmp
     *  handling: each try allocates a frame and registers it with the runtime,
     *  and `throw` longjmps back to the most recently registered frame. */
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

        BlockPtr getTryBlock() const { return tryBlock; }
        const std::vector<CatchClause>& getCatchClauses() const { return catchClauses; }
        BlockPtr getFinallyBlock() const { return finallyBlock; }

        void resolveTypes(CajetaModulePtr module) override;
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // One labeled group in a switch (`case 1: case 2: stmts...`). An empty
    // `caseValues` with isDefault set is the `default:` group.
    struct SwitchGroup {
        std::vector<ExpressionPtr> caseValues;
        bool isDefault = false;
        std::vector<BlockStatementPtr> statements;
    };

    /** SWITCH parExpression '{' switchBlockStatementGroup* switchLabel* '}' —
     *  a classic switch over an integer subject, groups falling through unless
     *  `break` or `return` ends them. `case X -> body` is out of scope. */
    class SwitchStatement : public Statement {
    private:
        ExpressionPtr subject;
        std::vector<SwitchGroup> groups;
    public:
        SwitchStatement(antlr4::Token* token, ExpressionPtr subject,
                        std::vector<SwitchGroup> groups)
            : Statement(token), subject(subject), groups(std::move(groups)) { }

        ExpressionPtr getSubject() const { return subject; }
        const std::vector<SwitchGroup>& getGroups() const { return groups; }

        void resolveTypes(CajetaModulePtr module) override;
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** SYNCHRONIZED parExpression block */
    class SynchronizedStatement : public Statement {
    private:
        ExpressionPtr parExpression;
        BlockPtr block;
    public:
        SynchronizedStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** RETURN expression? ';' */
    class ReturnStatement : public Statement {
    private:
        ExpressionPtr expression;
        bool modeCarrying = false;
    public:
        ReturnStatement(antlr4::Token* token, ExpressionPtr expression = nullptr,
                        bool modeCarrying = false)
            : Statement(token), expression(expression),
              modeCarrying(modeCarrying) { }

        // `return #= x` releases WHATEVER title this frame holds, as a runtime
        // bit — unlike `return x`, which carries the caller's mode transparently,
        // and `return #x`, which declares the transfer contract without asserting.
        bool isModeCarrying() const { return modeCarrying; }

        // Like ExpressionStatement, the returned expression isn't in `children`.
        void resolveTypes(CajetaModulePtr module) override;

        // Exposed for external walkers; the expression is not in `children`.
        ExpressionPtr getExpression() const { return expression; }

        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** THROW expression ';' */
    class ThrowStatement : public Statement {
    private:
        ExpressionPtr expression;
    public:
        ThrowStatement(antlr4::Token* token, ExpressionPtr expression = nullptr)
            : Statement(token), expression(expression) { }

        ExpressionPtr getExpression() const { return expression; }

        void resolveTypes(CajetaModulePtr module) override;
        void forEachSubNode(
            const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** BREAK identifier? ';' */
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

    /** CONTINUE identifier? ';' */
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

    /** YIELD expression ';' // Java17 */
    class YieldStatement : public Statement {
    private:
    public:
        YieldStatement(antlr4::Token* token) : Statement(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

//    TODO: Java17 switch expressions — guarded patterns and `case X ->` rules.

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