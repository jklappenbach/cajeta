//
// Created by James Klappenbach on 3/19/22.
//

#pragma once

#include "CajetaParser.h"
#include <string>
#include "cajeta/ast/AbstractSyntaxTree.h"

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
    class CajetaType;
    class Field;
    class FormalParameter;
    class Block;
    class Annotation;

    class Expression : public AbstractSyntaxTree {
        static Expression* fromContext(CajetaParser::ExpressionContext* ctxExpression);
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

    };

    class ParExpression : public PrimaryExpression {
        Expression* expression;
    };

    class ThisExpression : public PrimaryExpression {
        Field* field;
        CajetaType* type;
    };

    class LiteralExpression : public PrimaryExpression {
        string literal;
    };

    enum ReservedIdentifiers { UNKNOWN = -1, MODULE, REQUIRE, EXPORTS, OPENS, TO, USES, PROVIDES, WITH, TRANSITIVE, YIELD, SEALED, PERMITS, RECORD, VAR };

    class IdentifierExpression : public PrimaryExpression {
    private:
        string identifier;
    public:
        IdentifierExpression(string identifier) {
            this->identifier = identifier;
        }

        llvm::Value* codegen(ParseContext* ctxParse) override {
            //return ctxParse->builder->
            return nullptr;
        }

        static IdentifierExpression* fromContext(CajetaParser::IdentifierContext* ctxIdentifier) {
            IdentifierExpression* result = new IdentifierExpression(ctxIdentifier->getText());
            return result;
        }

    };

    class ClassExpression : public PrimaryExpression {
        CajetaType* type;

    };

    /**
     *  nonWildcardTypeArguments (explicitGenericInvocationSuffix | THIS arguments)
     */
    class GenericsExpression : public PrimaryExpression {
        list<CajetaType*> types;
    };


    class BopExpression : public Expression {
        Expression* expression;

    };

    class BopIdentifierExpression : public BopExpression {

    };

    class BopMethodCallExpression : public BopExpression {

    };

    class BopThisExpression : public BopExpression {

    };

    class BopNewExpression : public BopExpression {

    };

    class BopSuperExpression : public BopExpression {

    };

    class BopExplicitGenericInvocation : public BopExpression {

    };

    class ArrayExpression : public Expression {

    };

    class MethodCallExpression : public Expression {

    };

    class NewExpression : public Expression {

    };

    /**
     * '(' annotation* typeType ('&' typeType)* ')' expression
     */
    class CastExpression : public Expression {

    };

    /**
     * expression postfix=('++' | '--')
     */
    class PostfixExpression : public Expression {
        Expression* expression;
    };

    /**
     * prefix=('+'|'-'|'++'|'--') expression
     */
    class ArithmeticPrefixExpresssion : public Expression {
        string op;
        Expression* expression;
    };

    /**
     * prefix=('~'|'!') expression
     */
    class LogicalPrefixExpression : public Expression {
        string op;
        Expression* expression;
    };

    /**
     * expression ('<' '<' | '>' '>' '>' | '>' '>') expression
     */
    class BitshiftExpression : public Expression {
        Expression* lhs;
        char op[1];
        Expression* rhs;
    };

    /**
     * expression bop=('<=' | '>=' | '>' | '<') expression
     */
    class ComparisonExpression : public Expression {
        Expression* lhs;
        char op[2];
        Expression* rhs;
    };

    /**
     * expression bop=INSTANCEOF (typeType | pattern)
     */
    class InstanceOfExpression : public Expression {
        Expression* expression;
        CajetaType* type;
        string pattern;
    };

    /**
     * expression bop=('==' | '!=') expression
     */
    class EquivalenceExpression : public Expression {
        Expression* lhs;
        char op[3];
        Expression* rhs;
    };

    /**
     * expression bop='&' expression
     */
    class BitwiseAndExpression : public Expression {
        Expression* lhs;
        Expression* rhs;
    };

    /**
    * expression bop='^' expression
    */
    class BitwiseExOrExpression : public Expression {
        Expression* lhs;
        Expression* rhs;
    };

    /**
    * expression bop = '|' expression
    */
    class BitwiseInclusiveOrExpression : public Expression {
        Expression* lhs;
        Expression* rhs;
    };


    /**
    * expression bop = '&&' expression
    */
    class LogicalAndExpression : public Expression {
        Expression* lhs;
        Expression* rhs;
    };

    /**
     * expression bop = '||' expression
     */
    class LogicalOrExpression : public Expression {
        Expression* lhs;
        Expression* rhs;
    };

    /**
     * <assoc=right> expression bop='?' expression ':' expression
     */
    class BooleanSwitchExpression : public Expression {
        Expression* lhs;
        Expression* rhs;
    };

    /**
     * <assoc=right> expression bop=('=' | '+=' | '-=' | '*=' | '/=' | '&=' | '|=' | '^=' | '>>=' | '>>>=' | '<<=' | '%=') expression
     */
    class AssocRightBinaryOpExpression : public Expression {
        Expression* lhs;
        char op[4];
        Expression* rhs;
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
        Expression* expression;
    };

    class LambdaBlockBody : public LambdaBody {
        Block* block;
    };

    /**
     * lambdaExpression // Java8
     */
    class LambdaExpression : public Expression {
        LambdaParameters* lambdaParameters;
        LambdaBody* lambdaBody;
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
        Expression* expression;

    };

}
