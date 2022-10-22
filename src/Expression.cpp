//
// Created by James Klappenbach on 3/19/22.
//

#include "cajeta/ast/Expression.h"

namespace cajeta {
    Expression* Expression::fromContext(CajetaParser::ExpressionContext* ctxExpression) {
        Expression* expression = NULL;
        if (ctxExpression->primary()) {

        } else if (!ctxExpression->expression().empty()) {

        } else if (ctxExpression->methodCall()) {

        } else if (ctxExpression->NEW()) {

        } else if (ctxExpression->LPAREN()) {

        } else if (!ctxExpression->annotation().empty()) {
        } else if (ctxExpression->prefix) {
        } else if (ctxExpression->creator()) {

        } else if (!ctxExpression->typeType().empty()) {

        } else if (ctxExpression->RPAREN()) {

        } else if (!ctxExpression->BITAND().empty()) {

        } else if (ctxExpression->AND()) {

        } else if (ctxExpression->SUB()) {

        } else if (ctxExpression->INC()) {

        } else if (ctxExpression->DEC()) {

        } else if (ctxExpression->TILDE()) {

        } else if (ctxExpression->BANG()) {

        } else if (ctxExpression->lambdaExpression()) {

        } else if (ctxExpression->switchExpression()) {

        } else if (ctxExpression->identifier()) {

        } else if (ctxExpression->typeArguments()) {
        } else if (ctxExpression->classType()) {
        } else if (ctxExpression->MUL()) {
        } else if (ctxExpression->DIV()) {
        } else if (ctxExpression->MOD()) {
        } else if (!ctxExpression->LT().empty()) {
        } else if (!ctxExpression->GT().empty()) {
        } else if (ctxExpression->LE()) {
        } else if (ctxExpression->GE()) {
        } else if (ctxExpression->EQUAL()) {
        } else if (ctxExpression->NOTEQUAL()) {
        } else if (ctxExpression->CARET()) {
        } else if (ctxExpression->BITOR()) {
        } else if (ctxExpression->AND()) {
        } else if (ctxExpression->OR()) {
        } else if (ctxExpression->COLON()) {
        } else if (ctxExpression->QUESTION()) {
        } else if (ctxExpression->ASSIGN()) {
        } else if (ctxExpression->ADD_ASSIGN()) {
        } else if (ctxExpression->SUB_ASSIGN()) {
        } else if (ctxExpression->MUL_ASSIGN()) {
        } else if (ctxExpression->DIV_ASSIGN()) {
        } else if (ctxExpression->AND_ASSIGN()) {
        } else if (ctxExpression->OR_ASSIGN()) {
        } else if (ctxExpression->XOR_ASSIGN()) {
        } else if (ctxExpression->RSHIFT_ASSIGN()) {
        } else if (ctxExpression->URSHIFT_ASSIGN()) {
        } else if (ctxExpression->LSHIFT_ASSIGN()) {
        } else if (ctxExpression->MOD_ASSIGN()) {
        } else if (ctxExpression->DOT()) {
        } else if (ctxExpression->THIS()) {
        } else if (ctxExpression->innerCreator()) {
        } else if (ctxExpression->SUPER()) {
        } else if (ctxExpression->superSuffix()) {
        } else if (ctxExpression->explicitGenericInvocation()) {
        } else if (ctxExpression->nonWildcardTypeArguments()) {
        } else if (ctxExpression->LBRACK()) {
        } else if (ctxExpression->RBRACK()) {
        } else if (ctxExpression->INSTANCEOF()) {
        } else if (ctxExpression->pattern()) {
        }

        return expression;
    }
}

