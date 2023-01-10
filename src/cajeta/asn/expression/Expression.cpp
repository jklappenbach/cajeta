//
// Created by James Klappenbach on 3/19/22.
//

#include "Expression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/util/LiteralUtils.h"
#include "cajeta/util/MemoryManager.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/error/ExplicitCastRequiredException.h"
#include "cajeta/error/InvalidOperandException.h"
#include "BinaryOpExpression.h"
#include "DotExpression.h"
#include "LiteralExpression.h"
#include "MethodCallExpression.h"
#include "NewExpression.h"

namespace cajeta {
    ExpressionPtr Expression::fromContext(CajetaParser::ExpressionContext* ctx) {
        antlr4::Token* token = ctx->getStart();
        ExpressionPtr result = nullptr;
        if (ctx->ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_ASSIGN, token);
        } else if (ctx->primary()) {
            result = PrimaryExpression::fromContext(ctx->primary());
        } else if (ctx->methodCall()) {
            result = make_shared<MethodCallExpression>(ctx->methodCall(), token);
        } else if (ctx->NEW()) {
            result = make_shared<NewExpression>(ctx->creator(), token);
        } else if (ctx->DOT()) {
            result = make_shared<DotExpression>(ctx, token);
        } else if (ctx->identifier()) {
            result = make_shared<IdentifierExpression>(ctx->identifier(), ctx->primary() != nullptr);
        } else if (ctx->LPAREN()) {
        } else if (ctx->LBRACK()) {
            result = make_shared<ArrayIndexExpression>(ctx, token);
        } else if (!ctx->annotation().empty()) {

        } else if (ctx->creator()) {
        } else if (!ctx->typeType().empty()) {

        } else if (ctx->RPAREN()) {

        } else if (!ctx->BITAND().empty()) {

        } else if (ctx->AND()) {
        } else if (ctx->ADD()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_ADD, token);
        } else if (ctx->SUB()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_SUB, token);
        } else if (ctx->MUL()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_MUL, token);
        } else if (ctx->DIV()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_DIV, token);
        } else if (ctx->MOD()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_MOD, token);
        } else if (ctx->INC()) {
            if (ctx->prefix) {
                result = make_shared<PrefixExpression>(PREFIX_OP_INC, token);
            } else {
                result = make_shared<PostfixExpression>(POSTFIX_OP_INC, token);
            }
        } else if (ctx->DEC()) {
            if (ctx->prefix) {
                result = make_shared<PrefixExpression>(PREFIX_OP_DEC, token);
            } else {
                result = make_shared<PostfixExpression>(POSTFIX_OP_DEC, token);
            }
        } else if (ctx->TILDE()) {

        } else if (ctx->BANG()) {

        } else if (ctx->lambdaExpression()) {

        } else if (ctx->switchExpression()) {

        } else if (ctx->typeArguments()) {

        } else if (ctx->classType()) {

        } else if (!ctx->LT().empty()) {

        } else if (!ctx->GT().empty()) {

        } else if (ctx->LE()) {

        } else if (ctx->GE()) {

        } else if (ctx->EQUAL()) {
            cout << "Hit!";
        } else if (ctx->NOTEQUAL()) {

        } else if (ctx->CARET()) {

        } else if (ctx->BITOR()) {

        } else if (ctx->AND()) {

        } else if (ctx->OR()) {

        } else if (ctx->COLON()) {

        } else if (ctx->QUESTION()) {

        } else if (ctx->ADD_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_ADD_EQUALS, token);
        } else if (ctx->SUB_ASSIGN()) {

        } else if (ctx->MUL_ASSIGN()) {

        } else if (ctx->DIV_ASSIGN()) {

        } else if (ctx->AND_ASSIGN()) {

        } else if (ctx->OR_ASSIGN()) {

        } else if (ctx->XOR_ASSIGN()) {

        } else if (ctx->RSHIFT_ASSIGN()) {

        } else if (ctx->URSHIFT_ASSIGN()) {

        } else if (ctx->LSHIFT_ASSIGN()) {

        } else if (ctx->MOD_ASSIGN()) {

        } else if (ctx->THIS()) {

        } else if (ctx->innerCreator()) {

        } else if (ctx->SUPER()) {

        } else if (ctx->superSuffix()) {

        } else if (ctx->explicitGenericInvocation()) {

        } else if (ctx->nonWildcardTypeArguments()) {

        } else if (ctx->LBRACK()) {

        } else if (ctx->RBRACK()) {

        } else if (ctx->INSTANCEOF()) {

        } else if (ctx->pattern()) {
        }

        if (result) {
            if (!ctx->expression().empty()) {
                for (auto childContext: ctx->expression()) {
                    result->addChild(Expression::fromContext(childContext));
                }
            }
        }
        return result;
    }

    ArrayIndexExpression::ArrayIndexExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(
        token) {

    }

    llvm::Value* ArrayIndexExpression::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());

        llvm::Value* fieldAllocation = children[0]->generateCode(module);
        llvm::Constant* arrayIndex = nullptr;
        for (int i = 1; i < children.size(); i++) {
            llvm::Constant* dimensionValue = (llvm::Constant*) children[i]->generateCode(module);
            if (arrayIndex == nullptr) {
                arrayIndex = dimensionValue;
            } else {
                arrayIndex = llvm::ConstantExpr::getMul(dimensionValue, arrayIndex);
            }
        }
        vector<llvm::Value*> indexes({arrayIndex});
        llvm::Type* llvmFieldType = ((llvm::AllocaInst*) fieldAllocation)->getAllocatedType();
        llvm::ArrayType* llvmArrayType = (llvm::ArrayType*) llvmFieldType->getContainedType(0);
        llvm::Type* llvmElementType = llvmArrayType->getElementType();
        llvm::Value* llvmArray = module->getBuilder()->CreateStructGEP(llvmArrayType, fieldAllocation, 0);
        llvm::Value* result = module->getBuilder()->CreateGEP(llvmElementType, llvmArray, llvm::ArrayRef<llvm::Value*>(indexes));
        module->getAsnStack().pop_back();
        return result;
    }


//    antlr4::tree::TerminalNode *LPAREN();
//    ExpressionContext *expression();
//    antlr4::tree::TerminalNode *RPAREN();
//    antlr4::tree::TerminalNode *THIS();
//    antlr4::tree::TerminalNode *SUPER();
//    LiteralContext *literal();
//    IdentifierContext *identifier();
//    TypeTypeOrVoidContext *typeTypeOrVoid();
//    antlr4::tree::TerminalNode *DOT();
//    antlr4::tree::TerminalNode *CLASS();
//    NonWildcardTypeArgumentsContext *nonWildcardTypeArguments();
//    ExplicitGenericInvocationSuffixContext *explicitGenericInvocationSuffix();
//    ArgumentsContext *arguments();
    ExpressionPtr PrimaryExpression::fromContext(CajetaParser::PrimaryContext* ctx) {
        ExpressionPtr result = nullptr;
        if (ctx->LPAREN()) {
            result = Expression::fromContext(ctx->expression());
        } else if (ctx->literal()) {
            result = LiteralExpression::fromContext(ctx->literal());
        } else if (ctx->identifier()) {
            result = make_shared<IdentifierExpression>(ctx->identifier(), true);
        } else if (ctx->THIS()) {
            result = make_shared<ThisExpression>(ctx->expression());
        }
        return result;
    }

    llvm::Value* PrimaryExpression::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }
}
