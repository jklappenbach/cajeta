//
// CajetaConstantType — see header.
//

#include "CajetaConstantType.h"

#include <algorithm>

#include "llvm/ADT/APInt.h"

namespace cajeta {

    CajetaConstantType::CajetaConstantType(int64_t value)
        : CajetaType(QualifiedName::getOrCreate(std::to_string(value)),
                     /*llvmType=*/nullptr,
                     CONSTANT_FLAG | PRIMITIVE_FLAG),
          value(value) {}

    int64_t CajetaConstantType::parseLiteral(
            CajetaParser::IntegerLiteralContext* ctx) {
        // Mirror IntegerLiteralExpression::generateCode's radix handling.
        uint8_t radix = 10;
        size_t prefixLen = 0;
        if (ctx->BINARY_LITERAL()) { radix = 2;  prefixLen = 2; }   // "0b"/"0B"
        else if (ctx->HEX_LITERAL()) { radix = 16; prefixLen = 2; } // "0x"/"0X"
        else if (ctx->OCT_LITERAL()) { radix = 8;  prefixLen = 0; } // leading 0 ok for base-8
        // else decimal

        std::string text = ctx->getText();
        if (prefixLen && text.size() >= prefixLen) {
            text.erase(0, prefixLen);
        }
        if (!text.empty()) {
            char last = text.back();
            if (last == 'l' || last == 'L') text.pop_back();
        }
        text.erase(std::remove(text.begin(), text.end(), '_'), text.end());

        if (text.empty()) return 0;
        llvm::APInt apint(64, text, radix);
        return apint.getSExtValue();
    }

    CajetaTypePtr CajetaConstantType::of(int64_t value) {
        return std::make_shared<CajetaConstantType>(value);
    }

}
