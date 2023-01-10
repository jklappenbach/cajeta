//
// Created by James Klappenbach on 4/14/23.
//

#pragma once

#include "Expression.h"

namespace cajeta {

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

        static ExpressionPtr fromContext(CajetaParser::LiteralContext* ctx);
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

        llvm::Value* generateCode(CajetaModulePtr module) override;
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
        IntegerLiteralExpression(CajetaParser::IntegerLiteralContext* ctx) : LiteralExpression(ctx->getStart()) {
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

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    enum FloatLiteralType {
        FLOAT_LITERAL_FLOAT, FLOAT_LITERAL_HEX
    };

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

} // code