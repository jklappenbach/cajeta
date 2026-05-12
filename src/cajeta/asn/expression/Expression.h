//
// Created by James Klappenbach on 3/19/22.
//

#pragma once

#include <list>
#include <string>
#include "../AbstractSyntaxNode.h"
#include "CajetaParser.h"
#include "../../type/CajetaType.h"

using namespace std;

/**
expression
    : primary
    | REFERENCE primary
    | expression bop='.'
      (
         identifier
       | methodCall
       | THIS
       | NEW nonWildcardTypeArguments? innerCreator
       | SUPER superSuffix
       | explicitGenericInvocation
      )
    | expression '[' expression ']'
    | methodCall
    | NEW creator
    | '(' annotation* typeType ('&' typeType)* ')' expression
    | expression postfix=('++' | '--')
    | prefix=('+'|'-'|'++'|'--') expression
    | prefix=('~'|'!') expression
    | expression bop=('*'|'/'|'%') expression
    | expression bop=('+'|'-') expression
    | expression ('<' '<' | '>' '>' '>' | '>' '>') expression
    | expression bop=('<=' | '>=' | '>' | '<') expression
    | expression bop=INSTANCEOF (typeType | pattern)
    | expression bop=('==' | '!=') expression
    | expression bop='&' expression
    | expression bop='^' expression
    | expression bop='|' expression
    | expression bop='&&' expression
    | expression bop='||' expression
    | <assoc=right> expression bop='?' expression ':' expression
    | <assoc=right> expression
      bop=('=' | '+=' | '-=' | '*=' | '/=' | '&=' | '|=' | '^=' | '>>=' | '>>>=' | '<<=' | '%=')
      expression
    | lambdaExpression // Java8
    | switchExpression // Java17

    // Java 8 methodReference
    | expression '::' typeArguments? identifier
    | typeType '::' (typeArguments? identifier | NEW)
    | classType '::' typeArguments? NEW
    ;
*/

namespace cajeta {
    class CajetaModule;

    class CajetaType;

    class Field;

    class FormalParameter;

    class Block;

    class Annotation;

    class Expression;
    typedef shared_ptr<Expression> ExpressionPtr;

    // Expression is a sibling of Statement under AbstractSyntaxNode. When an expression
    // appears in statement position (e.g. `foo();`), wrap it in ExpressionStatement
    // rather than relying on inheritance — see Statement::fromContext.
    class Expression : public AbstractSyntaxNode {
    protected:
        bool primary;
        // Set by the type-resolver pass; codegen consults this for situations where the
        // LLVM type alone is insufficient (e.g. fp8 stored as i8). May be null pre-resolution.
        CajetaTypePtr resolvedType;
    public:
        Expression(antlr4::Token* token) : AbstractSyntaxNode(token) { }

        Expression(bool primary, antlr4::Token* token) : AbstractSyntaxNode(token) {
            this->primary = primary;
        }

        CajetaTypePtr getResolvedType() const { return resolvedType; }
        void setResolvedType(CajetaTypePtr t) { resolvedType = t; }

        virtual void addChild(ExpressionPtr expression) {
            children.push_back(expression);
        };

        static ExpressionPtr fromContext(CajetaParser::ExpressionContext* ctx);
    };

    /**
        : '(' expression ')'
        | THIS
        | SUPER
        | literal
        | identifier
        | typeTypeOrVoid '.' CLASS
        | nonWildcardTypeArguments (explicitGenericInvocationSuffix | THIS arguments)
        ;
     */
    class PrimaryExpression : public Expression {
    public:
        PrimaryExpression(antlr4::Token* token) : Expression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;

        static ExpressionPtr fromContext(CajetaParser::PrimaryContext* ctx);
    };

    class ThisExpression : public PrimaryExpression {
    public:
        ThisExpression(CajetaParser::ExpressionContext* ctx) : PrimaryExpression(ctx->getStart()) { }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    enum ReservedIdentifiers {
        UNKNOWN = -1,
        MODULE,
        REQUIRE,
        EXPORTS,
        OPENS,
        TO,
        USES,
        PROVIDES,
        WITH,
        TRANSITIVE,
        YIELD,
        SEALED,
        PERMITS,
        RECORD,
        VAR
    };

    // Removed dead classes: ClassExpression, GenericsExpression, BopExpression and its 6
    // subclasses, ParExpression. Their grammar productions are handled by DotExpression
    // or are unreached. Member access lowers via DotExpression directly.

    class ArrayIndexExpression : public Expression {
    public:
        ArrayIndexExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };


    /**
     * '(' annotation* typeType ('&' typeType)* ')' expression
     */
    class CastExpression : public Expression {
    private:
        CajetaTypePtr destType;
    public:
        CastExpression(CajetaTypePtr destType, antlr4::Token* token)
            : Expression(token), destType(destType) { }

        void resolveTypes(CajetaModulePtr module) override {
            AbstractSyntaxNode::resolveTypes(module);
            resolvedType = destType;
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * expression postfix=('++' | '--')
     */
    enum PostfixOp {
        POSTFIX_OP_INC, POSTFIX_OP_DEC
    };

    class PostfixExpression : public Expression {
        PostfixOp op;
    public:
        PostfixExpression(PostfixOp op, antlr4::Token* token) : Expression(token) {
            this->op = op;
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * prefix=('+'|'-'|'++'|'--'|'~'|'!') expression
     */
    enum PrefixOp {
        PREFIX_OP_POSITIVE,
        PREFIX_OP_NEGATIVE,
        PREFIX_OP_INC,
        PREFIX_OP_DEC,
        PREFIX_OP_BITNOT,   // ~
        PREFIX_OP_LOGNOT    // !
    };

    class PrefixExpression : public Expression {
    private:
        PrefixOp op;
    public:
        PrefixExpression(PrefixOp op, antlr4::Token* token) : Expression(token) {
            this->op = op;
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // Removed dead classes: LogicalPrefixExpression, BitshiftExpression, ComparisonExpression,
    // EquivalenceExpression, BitwiseAndExpression, BitwiseExOrExpression,
    // BitwiseInclusiveOrExpression, LogicalAndExpression, LogicalOrExpression,
    // ArithmeticAssignmentExpression. All of these are handled by BinaryOpExpression
    // variants now (shift / comparison / equality / bitwise / logical / *_EQUALS).
    // PrefixExpression handles ~ / ! / +/- / ++/--.

    /**
     * <assoc=right> expression bop='?' expression ':' expression
     *
     * Ternary. children = [cond, then, else] from fromContext's child-population loop.
     * Emits a conditional branch + phi pattern, same shape as &&/||.
     */
    class BooleanSwitchExpression : public Expression {
    public:
        BooleanSwitchExpression(antlr4::Token* token) : Expression(token) { }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * expression bop=INSTANCEOF (typeType | pattern)
     *
     * Compile-time only for now: emits a constant i1 by comparing the lhs's
     * resolvedType to the target type. A real runtime check needs class-hierarchy
     * metadata emitted via StructureMetadata (so we can walk parents at runtime);
     * that's tracked separately.
     */
    class InstanceOfExpression : public Expression {
    private:
        CajetaTypePtr type;
        string pattern;
    public:
        InstanceOfExpression(CajetaTypePtr type, string pattern, antlr4::Token* token) : Expression(token) {
            this->type = type;
            this->pattern = pattern;
        }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };


    // Placeholder for expression forms recognized by the grammar but not yet implemented
    // (lambdas, switch expressions, super dispatch, inner-class new, method references).
    // The parser produces one of these so the failure surfaces at codegen time as a clear
    // cajeta::Exception with the construct name and source location, rather than a silent
    /**
     * Java-17 switch expression. Today we only support the arrow form with single-
     * expression case bodies — `case X -> expr;` and `default -> expr;` — which is
     * the most common shape and avoids needing `yield`.
     *
     *   switch (x) {
     *     case 1, 2 -> 10;
     *     case 3    -> 20;
     *     default   -> 99;
     *   }
     *
     * Each case may match multiple constants via a comma-separated list. The cases
     * lower to an `llvm::SwitchInst` and a phi node collects each arm's value.
     */
    class SwitchExpression : public Expression {
    public:
        struct Case {
            list<ExpressionPtr> labels;     // empty == default
            ExpressionPtr body;
        };
    private:
        ExpressionPtr discriminator;
        list<Case> cases;
    public:
        SwitchExpression(antlr4::Token* token,
                          ExpressionPtr discriminator,
                          list<Case> cases)
            : Expression(token),
              discriminator(std::move(discriminator)),
              cases(std::move(cases)) { }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // nullptr returning invalid IR.
    class UnsupportedExpression : public Expression {
    private:
        string constructName;
    public:
        UnsupportedExpression(string constructName, antlr4::Token* token)
            : Expression(token), constructName(std::move(constructName)) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

}
