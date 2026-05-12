//
// Created by James Klappenbach on 4/14/23.
//

#include "LiteralExpression.h"
#include "../../compile/CajetaModule.h"
#include <cmath>
#include <cstdlib>

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

    void TextLiteralExpression::resolveTypes(CajetaModulePtr module) {
        switch (literalType) {
            case LITERAL_TYPE_BOOL:        resolvedType = CajetaType::of("boolean"); break;
            case LITERAL_TYPE_STRING:
            case LITERAL_TYPE_TEXT_BLOCK:
            case LITERAL_TYPE_NULL:        resolvedType = CajetaType::of("pointer"); break;
            default: break;
        }
    }

    // Strip the surrounding quote pair on a string literal so the bytes emitted into the
    // global include only the content. ANTLR's getText() returns the raw lexeme.
    static string stripQuotes(const string& raw) {
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
            return raw.substr(1, raw.size() - 2);
        }
        return raw;
    }

    llvm::Value* TextLiteralExpression::generateCode(CajetaModulePtr module) {
        auto& ctx = *module->getLlvmContext();
        switch (literalType) {
            case LITERAL_TYPE_BOOL:
                return value == "true"
                    ? llvm::ConstantInt::getTrue(ctx)
                    : llvm::ConstantInt::getFalse(ctx);
            case LITERAL_TYPE_NULL:
                return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0));
            case LITERAL_TYPE_STRING:
            case LITERAL_TYPE_TEXT_BLOCK:
                return module->getBuilder()->CreateGlobalStringPtr(stripQuotes(value), "str");
            default:
                return nullptr;
        }
    }

    void IntegerLiteralExpression::resolveTypes(CajetaModulePtr module) {
        // Default integer literals to int32; widening happens in BinaryOpExpression as needed.
        resolvedType = CajetaType::of("int32");
    }

    // Pick float32 if the literal carries an f/F suffix; float64 otherwise (matches the
    // d/D suffix or unsuffixed default).
    static bool hasFloat32Suffix(const string& s) {
        if (s.empty()) return false;
        char last = s.back();
        return last == 'f' || last == 'F';
    }

    void FloatLiteralExpression::resolveTypes(CajetaModulePtr module) {
        resolvedType = CajetaType::of(hasFloat32Suffix(value) ? "float32" : "float64");
    }

    llvm::Value* FloatLiteralExpression::generateCode(CajetaModulePtr module) {
        bool isFloat32 = hasFloat32Suffix(value);
        // Strip the trailing f/F/d/D suffix so APFloat doesn't choke on it.
        string numericText = value;
        if (!numericText.empty()) {
            char last = numericText.back();
            if (last == 'f' || last == 'F' || last == 'd' || last == 'D') {
                numericText.pop_back();
            }
        }
        const llvm::fltSemantics& sem = isFloat32 ? llvm::APFloat::IEEEsingle()
                                                  : llvm::APFloat::IEEEdouble();
        llvm::APFloat apf(sem);
        // APFloat handles both decimal and hex-float syntax (0x1.0p3, etc.) via this overload.
        auto status = apf.convertFromString(numericText, llvm::APFloat::rmNearestTiesToEven);
        if (!status) {
            // Parse failure; surface a zero constant rather than crashing.
            return llvm::ConstantFP::getZero(
                isFloat32 ? llvm::Type::getFloatTy(*module->getLlvmContext())
                          : llvm::Type::getDoubleTy(*module->getLlvmContext()));
        }
        return llvm::ConstantFP::get(*module->getLlvmContext(), apf);
    }

    llvm::Value* IntegerLiteralExpression::generateCode(CajetaModulePtr module) {
        uint8_t radix;
        switch (integerLiteralType) {
            case INTEGER_LITERAL_TYPE_BINARY: radix = 2;  break;
            case INTEGER_LITERAL_TYPE_OCT:    radix = 8;  break;
            case INTEGER_LITERAL_TYPE_HEX:    radix = 16; break;
            default:                          radix = 10; break;
        }

        // Default integer literals to 64-bit storage so any value parses correctly; the
        // ReturnStatement / BinaryOpExpression boundary code (and CajetaType::normalize
        // when we get to it) coerces to the surrounding context's expected width.
        // resolveTypes pins the Cajeta type to int32 for the purpose of type inference,
        // which downstream coercion uses.
        llvm::Type* valueType = llvm::IntegerType::getInt64Ty(*module->getLlvmContext());
        llvm::APInt apint(64, value, radix);
        return llvm::ConstantInt::get(valueType, apint);
    }
} // code