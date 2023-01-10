//
// Created by James Klappenbach on 4/14/23.
//

#include "LiteralExpression.h"
#include "../../compile/CajetaModule.h"

namespace cajeta {
    ExpressionPtr LiteralExpression::fromContext(CajetaParser::LiteralContext* ctx) {
        if (ctx->integerLiteral()) {
            return make_shared<IntegerLiteralExpression>(ctx->integerLiteral());
        } else if (ctx->floatLiteral()) {
            return make_shared<FloatLiteralExpression>(ctx->floatLiteral());
        } else {
            return make_shared<TextLiteralExpression>(ctx);
        }
    }

    llvm::Value* TextLiteralExpression::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* IntegerLiteralExpression::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());

        unsigned bits = 0;
        uint8_t radix;
        llvm::Type* valueType;

        switch (integerLiteralType) {
            case INTEGER_LITERAL_TYPE_BINARY:
                bits = value.length();
                radix = 2;
                break;
            case INTEGER_LITERAL_TYPE_OCT:
                bits = value.length() * 8;
                radix = 8;
                break;
            case INTEGER_LITERAL_TYPE_DECIMAL:
                bits = (int) (::log(atoi(value.c_str()))) + 1;
                radix = 10;
                break;
            case INTEGER_LITERAL_TYPE_HEX:
                bits = value.length() * 16;
                radix = 16;
                break;
        }

        if (bits == 1) {
            bits = 1;
            valueType = llvm::IntegerType::getInt1Ty(*module->getLlvmContext());
        } else if (bits > 1 && bits <= 8) {
            bits = 8;
            valueType = llvm::IntegerType::getInt8Ty(*module->getLlvmContext());
        } else if (bits > 8 && bits <= 16) {
            bits = 16;
            valueType = llvm::IntegerType::getInt16Ty(*module->getLlvmContext());
        } else if (bits > 16 && bits <= 32) {
            bits = 32;
            valueType = llvm::IntegerType::getInt32Ty(*module->getLlvmContext());
        } else if (bits > 32 && bits <= 64) {
            bits = 64;
            valueType = llvm::IntegerType::getInt64Ty(*module->getLlvmContext());
        } else {
            bits = 128;
            valueType = llvm::IntegerType::getInt128Ty(*module->getLlvmContext());
        }
        llvm::Value* result = llvm::ConstantInt::getIntegerValue(valueType, llvm::APInt(bits, value, radix));
        module->getAsnStack().pop_back();
        return result;
    }
} // code