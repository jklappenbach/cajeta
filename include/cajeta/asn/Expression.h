//
// Created by James Klappenbach on 3/19/22.
//

#pragma once

#include <list>
#include <string>
#include <cajeta/asn/Statement.h>
#include "CajetaParser.h"
#include <cajeta/type/CajetaType.h>

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

    class Expression : public Statement {
    protected:
        bool primary;
    public:
        Expression(antlr4::Token* token) : Statement(token) { }
        Expression(bool primary, antlr4::Token* token) : Statement(token) {
            this->primary = primary;
        }
        virtual void addChild(Expression* expression) {
            children.push_back(expression);
        };
        static Expression* fromContext(CajetaParser::ExpressionContext* ctx);
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
        llvm::Value* generateCode(CajetaModule* module) override;
        static Expression* fromContext(CajetaParser::PrimaryContext* ctx);
    };

    class ParExpression : public PrimaryExpression {
    public:
        ParExpression(antlr4::Token* token) : PrimaryExpression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }
    };

    class ThisExpression : public PrimaryExpression {
    private:
        Field* field;
        CajetaType* type;
    public:
        ThisExpression(CajetaParser::ExpressionContext* ctx) : PrimaryExpression(ctx->getStart()) {

        }
    };

    class DotExpression : public Expression {
        string identifier;
    public:
        DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);
        llvm::Value* generateCode(CajetaModule* module) override;
    };

    enum LiteralType {
        LITERAL_TYPE_BOOL,
        LITERAL_TYPE_CHAR,
        LITERAL_TYPE_NULL,
        LITERAL_TYPE_STRING,
        LITERAL_TYPE_TEXT_BLOCK,
        LITERAL_TYPE_INTEGER,
        LITERAL_TYPE_FLOAT
    };

    class LiteralExpression : public PrimaryExpression {
    protected:
        string value;
    public:
        LiteralExpression(antlr4::Token* token) : PrimaryExpression(token) { }
        static LiteralExpression* fromContext(CajetaParser::LiteralContext* ctx);
    };

    class TextLiteralExpression : public LiteralExpression {
    private:
        LiteralType literalType;
    public:
        TextLiteralExpression(CajetaParser::LiteralContext* ctx) : LiteralExpression(ctx->getStart()) {
            value = ctx->getText();
            if (ctx->BOOL_LITERAL()) {
                literalType = LITERAL_TYPE_BOOL;
            } else if (ctx->NULL_LITERAL()) {
                literalType = LITERAL_TYPE_NULL;
            } else if (ctx->STRING_LITERAL()) {
                literalType = LITERAL_TYPE_STRING;
            } else if (ctx->TEXT_BLOCK()) {
                literalType = LITERAL_TYPE_TEXT_BLOCK;
            }
        }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }
        // TODO: Add override when we have strings for getType

    };

    enum IntegerLiteralType {
        INTEGER_LITERAL_TYPE_BINARY,
        INTEGER_LITERAL_TYPE_DECIMAL,
        INTEGER_LITERAL_TYPE_HEX,
        INTEGER_LITERAL_TYPE_OCT
    };

    class IntegerLiteralExpression : public LiteralExpression {
    private:
        IntegerLiteralType integerLiteralType;
    public:
        IntegerLiteralExpression(CajetaParser::IntegerLiteralContext* ctx) : LiteralExpression(ctx->getStart()){
            value = ctx->getText();
            if (ctx->BINARY_LITERAL()) {
                integerLiteralType = INTEGER_LITERAL_TYPE_BINARY;
            } else if (ctx->HEX_LITERAL()) {
                integerLiteralType = INTEGER_LITERAL_TYPE_HEX;
            } else if (ctx->OCT_LITERAL()) {
                integerLiteralType = INTEGER_LITERAL_TYPE_OCT;
            } else {
                integerLiteralType = INTEGER_LITERAL_TYPE_DECIMAL;
            }
        }
        llvm::Value* generateCode(CajetaModule* module) override;
    };

    enum FloatLiteralType { FLOAT_LITERAL_FLOAT, FLOAT_LITERAL_HEX };
    class FloatLiteralExpression : public LiteralExpression {
    private:
        FloatLiteralType floatLiteralType;
    public:
        FloatLiteralExpression(CajetaParser::FloatLiteralContext* ctx) : LiteralExpression(ctx->getStart()) {
            if (ctx->HEX_FLOAT_LITERAL()) {
                floatLiteralType = FLOAT_LITERAL_HEX;
            } else {
                floatLiteralType = FLOAT_LITERAL_FLOAT;
            }
            value = ctx->getText();
        }
    };

    enum ReservedIdentifiers { UNKNOWN = -1, MODULE, REQUIRE, EXPORTS, OPENS, TO, USES, PROVIDES, WITH, TRANSITIVE, YIELD, SEALED, PERMITS, RECORD, VAR };

    class IdentifierExpression : public Expression {
    private:
        string identifier;
    public:
        IdentifierExpression(CajetaParser::IdentifierContext* ctx, bool primary) : Expression(ctx->getStart()) {
            this->primary = primary;
            identifier = ctx->getText();
        }
        IdentifierExpression(bool primary, CajetaParser::IdentifierContext* ctx) : Expression(primary, ctx->getStart()) {
            identifier = ctx->getText();
        }
        string& getTextValue() { return identifier; }
        llvm::Value* generateCode(CajetaModule* module) override;
    };

    class ClassExpression : public PrimaryExpression {
    private:
        CajetaType* type;
    public:
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }
    };

    /**
     *  nonWildcardTypeArguments (explicitGenericInvocationSuffix | THIS arguments)
     */
    class GenericsExpression : public PrimaryExpression {
    private:
        list<CajetaType*> types;
    public:
        GenericsExpression(antlr4::Token* token) : PrimaryExpression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }
    };


    class BopExpression : public Expression {
    private:
        Expression* expression;
    public:
        BopExpression(antlr4::Token* token) : Expression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {
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
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }

    };

    class BopMethodCallExpression : public BopExpression {
    public:
        BopMethodCallExpression(antlr4::Token* token) : BopExpression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }
    };

    class BopThisExpression : public BopExpression {
    public:
        BopThisExpression(antlr4::Token* token) : BopExpression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }

    };

    class BopNewExpression : public BopExpression {
    public:
        BopNewExpression(antlr4::Token* token) : BopExpression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }

    };

    class BopSuperExpression : public BopExpression {
    public:
        BopSuperExpression(antlr4::Token* token) : BopExpression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }

    };

    class BopExplicitGenericInvocation : public BopExpression {
    public:
        BopExplicitGenericInvocation(antlr4::Token* token) : BopExpression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }

    };

    class ArrayExpression : public Expression {
    public:
        ArrayExpression(antlr4::Token* token) : Expression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }

    };

    class MethodCallExpression : public Expression {
    public:
        MethodCallExpression(antlr4::Token* token) : Expression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }
    };

    class CreatorRest : public AbstractSyntaxNode {
    public:
        CreatorRest(antlr4::Token* token) : AbstractSyntaxNode(token) { }
        static CreatorRest* fromContext(CajetaParser::CreatorContext* ctx, antlr4::Token* token);
    };

    class ClassCreatorRest : public CreatorRest {
    public:
        ClassCreatorRest(CajetaParser::ClassCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            if (ctx->arguments()->parameterList()) {
                for (auto & expressionContext : ctx->arguments()->parameterList()->expression()) {
                    children.push_back(Expression::fromContext(expressionContext));
                }
            }
        }
        llvm::Value* generateCode(CajetaModule* module) override;
    };

    class ArrayCreatorRest : public CreatorRest {
    private:
    public:
        ArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            for (auto &expressionContext : ctx->expression()) {
                children.push_back(Expression::fromContext(expressionContext));
            }
        }
        llvm::Value* generateCode(CajetaModule* module) override;
    };

    class NewExpression : public Expression {
        string package;
        string typeName;
        CreatorRest* creatorRest;
    public:
        NewExpression(antlr4::Token* token) : Expression(token) { }

        NewExpression(CajetaParser::CreatorContext* creatorContext, antlr4::Token* token) : Expression(token) {
            if (creatorContext->createdName() != nullptr) {
                if (creatorContext->createdName()->primitiveType()) {
                    typeName = creatorContext->createdName()->primitiveType()->getText();
                } else if (!creatorContext->createdName()->identifier().empty()) {
                    int count = creatorContext->createdName()->identifier().size();
                    int n = 0;
                    for (auto &identifierPart : creatorContext->createdName()->identifier()) {
                        if (n++ == count - 1) {
                            typeName = identifierPart->getText();
                        } else {
                            package.append(identifierPart->getText());
                        }
                    }
                }
            }
            creatorRest = CreatorRest::fromContext(creatorContext, token);
        }

        llvm::Value* generateCode(CajetaModule* module) override;
    };

    /**
     * '(' annotation* typeType ('&' typeType)* ')' expression
     */
    class CastExpression : public Expression {
    public:
        CastExpression(antlr4::Token* token) : Expression(token) { }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }
    };

    /**
     * expression postfix=('++' | '--')
     */
    enum PostfixOp { POSTFIX_OP_INC, POSTFIX_OP_DEC };

    class PostfixExpression : public Expression {
        PostfixOp op;
    public:
        PostfixExpression(PostfixOp op, antlr4::Token* token) : Expression(token) {
            this->op = op;
        }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }
    };

    /**
     * prefix=('+'|'-'|'++'|'--') expression
     */
    enum PrefixOp { PREFIX_OP_POSITIVE, PREFIX_OP_NEGATIVE, PREFIX_OP_INC, PREFIX_OP_DEC };
    class PrefixExpression : public Expression {
    private:
        PrefixOp op;
    public:
        PrefixExpression(PrefixOp op, antlr4::Token* token) : Expression(token) {
            this->op = op;
        }
        llvm::Value* generateCode(CajetaModule* module) override {  return nullptr; }
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
    enum BitshiftOp { BITSHIFT_OP_LEFT, BITSHIFT_OP_UNSIGNED_RIGHT, BITSHIFT_OP_RIGHT };
    class BitshiftExpression : public Expression {
    private:
        BitshiftOp op;
    public:
        BitshiftExpression(antlr4::Token* token) : Expression(token) { }
    };

    /**
     * expression bop=('<=' | '>=' | '>' | '<') expression
     */
    enum ComparisonOp { COMPARE_OP_LTE, COMPARE_OP_GTE, COMPARE_OP_GT, COMPARE_OP_LT };
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
        CajetaType* type;
        string pattern;
    public:
        InstanceOfExpression(CajetaType* type, string pattern, antlr4::Token* token) : Expression(token) {
            this->type = type;
            this->pattern = pattern;
        }
    };

    /**
     * expression bop=('==' | '!=') expression
     */
    enum EquivalenceOp { EQUIVALENCE_OP_EQ, EQUIVALENCE_OP_NE };
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
     * <assoc=right> expression bop=('=' | '+=' | '-=' | '*=' | '/=' | '&=' | '|=' | '^=' | '>>=' | '>>>=' | '<<=' | '%=') expression
     */
    enum BinaryOp {
        BINARY_OP_ASSIGN,
        BINARY_OP_ADD_EQUALS,
        BINARY_OP_SUB_EQUALS,
        BINARY_OP_MUL_EQUALS,
        BINARY_OP_DIV_EQUALS,
        BINARY_OP_BITAND_EQUALS,
        BINARY_OP_BITOR_EQUALS,
        BINARY_OP_BITXOR_EQUALS,
        BINARY_OP_SHIFTRIGHT_EQUALS,
        BINARY_OP_USHIFTRIGHT_EQUALS,
        BINARY_OP_SHIFTLEFT_EQUALS,
        BINARY_OP_MOD_EQUALS
    };

    class BinaryOpExpression : public Expression {
    private:
        BinaryOp binaryOp;
    public:
        BinaryOpExpression(BinaryOp binaryOp, antlr4::Token* token) : Expression(token) {
            this->binaryOp = binaryOp;
        }

        llvm::Value* generateCode(CajetaModule* module) override;
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
