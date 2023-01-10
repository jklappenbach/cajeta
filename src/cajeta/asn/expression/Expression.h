//
// Created by James Klappenbach on 3/19/22.
//

#pragma once

#include <list>
#include <string>
#include "../Statement.h"
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

    class Expression : public Statement {
    protected:
        bool primary;
    public:
        Expression(antlr4::Token* token) : Statement(token) { }

        Expression(bool primary, antlr4::Token* token) : Statement(token) {
            this->primary = primary;
        }

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

    class ParExpression : public PrimaryExpression {
    public:
        ParExpression(antlr4::Token* token) : PrimaryExpression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }
    };

    class ThisExpression : public PrimaryExpression {
    private:
        Field* field;
        CajetaTypePtr type;
    public:
        ThisExpression(CajetaParser::ExpressionContext* ctx) : PrimaryExpression(ctx->getStart()) {

        }
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

    class ClassExpression : public PrimaryExpression {
    private:
        CajetaTypePtr type;
    public:
        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }
    };

    /**
     *  nonWildcardTypeArguments (explicitGenericInvocationSuffix | THIS arguments)
     */
    class GenericsExpression : public PrimaryExpression {
    private:
        list<CajetaType*> types;
    public:
        GenericsExpression(antlr4::Token* token) : PrimaryExpression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }
    };


    class BopExpression : public Expression {
    private:
        ExpressionPtr expression;
    public:
        BopExpression(antlr4::Token* token) : Expression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override {
            llvm::Value* base = children[0]->generateCode(module);
            llvm::Type* type = base->getType();
            try {
                string structName = type->getStructName().str();
            } catch (exception) {
                // We can only index into structures here.  We have the wrong type
            }
            return nullptr;
        }
    };

    class BopIdentifierExpression : public BopExpression {
    public:
        BopIdentifierExpression(antlr4::Token* token) : BopExpression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }

    };

    class BopMethodCallExpression : public BopExpression {
    public:
        BopMethodCallExpression(antlr4::Token* token) : BopExpression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }
    };

    class BopThisExpression : public BopExpression {
    public:
        BopThisExpression(antlr4::Token* token) : BopExpression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }

    };

    class BopNewExpression : public BopExpression {
    public:
        BopNewExpression(antlr4::Token* token) : BopExpression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }

    };

    class BopSuperExpression : public BopExpression {
    public:
        BopSuperExpression(antlr4::Token* token) : BopExpression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }

    };

    class BopExplicitGenericInvocation : public BopExpression {
    public:
        BopExplicitGenericInvocation(antlr4::Token* token) : BopExpression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }

    };

    class ArrayIndexExpression : public Expression {
    public:
        ArrayIndexExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };


    /**
     * '(' annotation* typeType ('&' typeType)* ')' expression
     */
    class CastExpression : public Expression {
    public:
        CastExpression(antlr4::Token* token) : Expression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }
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

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }
    };

    /**
     * prefix=('+'|'-'|'++'|'--') expression
     */
    enum PrefixOp {
        PREFIX_OP_POSITIVE, PREFIX_OP_NEGATIVE, PREFIX_OP_INC, PREFIX_OP_DEC
    };

    class PrefixExpression : public Expression {
    private:
        PrefixOp op;
    public:
        PrefixExpression(PrefixOp op, antlr4::Token* token) : Expression(token) {
            this->op = op;
        }

        llvm::Value* generateCode(CajetaModulePtr module) override { return nullptr; }
    };

    /**
     * prefix=('~'|'!') expression
     */
    class LogicalPrefixExpression : public Expression {
    private:
        string op;
    public:
        LogicalPrefixExpression(antlr4::Token* token) : Expression(token) { }
    };

    /**
     * expression ('<' '<' | '>' '>' '>' | '>' '>') expression
     */
    enum BitshiftOp {
        BITSHIFT_OP_LEFT, BITSHIFT_OP_UNSIGNED_RIGHT, BITSHIFT_OP_RIGHT
    };

    class BitshiftExpression : public Expression {
    private:
        BitshiftOp op;
    public:
        BitshiftExpression(antlr4::Token* token) : Expression(token) { }
    };

    /**
     * expression bop=('<=' | '>=' | '>' | '<') expression
     */
    enum ComparisonOp {
        COMPARE_OP_LTE, COMPARE_OP_GTE, COMPARE_OP_GT, COMPARE_OP_LT
    };

    class ComparisonExpression : public Expression {
    private:
        ComparisonOp op;
    public:
        ComparisonExpression(antlr4::Token* token) : Expression(token) { }
    };

    /**
     * expression bop=INSTANCEOF (typeType | pattern)
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
    };

    /**
     * expression bop=('==' | '!=') expression
     */
    enum EquivalenceOp {
        EQUIVALENCE_OP_EQ, EQUIVALENCE_OP_NE
    };

    class EquivalenceExpression : public Expression {
        EquivalenceOp op;

        EquivalenceExpression(EquivalenceOp op, antlr4::Token* token) : Expression(token) {
            this->op = op;
        }
    };

    /**
     * expression bop='&' expression
     */
    class BitwiseAndExpression : public Expression {
    public:
        BitwiseAndExpression(antlr4::Token* token) : Expression(token) { }
    };

    /**
    * expression bop='^' expression
    */
    class BitwiseExOrExpression : public Expression {
    public:
        BitwiseExOrExpression(antlr4::Token* token) : Expression(token) { }
    };

    /**
    * expression bop = '|' expression
    */
    class BitwiseInclusiveOrExpression : public Expression {
    public:
        BitwiseInclusiveOrExpression(antlr4::Token* token) : Expression(token) { }
    };


    /**
    * expression bop = '&&' expression
    */
    class LogicalAndExpression : public Expression {
    public:
        LogicalAndExpression(antlr4::Token* token) : Expression(token) { }
    };

    /**
     * expression bop = '||' expression
     */
    class LogicalOrExpression : public Expression {
    public:
        LogicalOrExpression(antlr4::Token* token) : Expression(token) { }
    };

    class ArithmeticAssignmentExpression : public Expression {
    public:
        ArithmeticAssignmentExpression(antlr4::Token* token) : Expression(token) { }

    };

    /**
     * <assoc=right> expression bop='?' expression ':' expression
     */
    class BooleanSwitchExpression : public Expression {
    public:
        BooleanSwitchExpression(antlr4::Token* token) : Expression(token) { }
    };


    /**
     * lambdaParameters
     *  : identifier
     *  | '(' formalParameterList? ')'
     *  | '(' identifier (',' identifier)* ')'
     *  | '(' lambdaLVTIList? ')'
     */
    class LambdaParameters {
        list<string> parIdentifiers;
    };

    class LambdaIdentifiersParameter : public LambdaParameters {
        string identifier;
    };

    class LambdaFormalParameters : public LambdaParameters {
        list<FormalParameter*> formalParameter;
    };

    class LambdaBody {

    };


    class LambdaExpressionBody : public LambdaBody {
        ExpressionPtr expression;
    };

    class LambdaBlockBody : public LambdaBody {
        Block* block;
    };

    /**
     * lambdaExpression // Java8
     */
    class LambdaExpression : public Expression {
    private:
        LambdaParameters* lambdaParameters;
        LambdaBody* lambdaBody;
    public:
        LambdaExpression(antlr4::Token* token) : Expression(token) { }
    };

//    | switchExpression // Java17

    /**
     * typeArgument
       : typeType
       | annotation* '?' ((EXTENDS | SUPER) typeType)?
       ;
     */
    class TypeArguments {
        Annotation* annotation;

    };

//    // Java 8 methodReference
//    | expression '::' typeArguments? identifier
//    | typeType '::' (typeArguments? identifier | NEW)
//    | classType '::' typeArguments? NEW
    class MethodReferenceExpression : public Expression {
    public:
        MethodReferenceExpression(antlr4::Token* token) : Expression(token) { }
    };

}
