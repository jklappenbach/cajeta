//
// Created by James Klappenbach on 4/14/23.
//

#include "LiteralExpression.h"
#include "../../compile/CajetaModule.h"
#include <algorithm>
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
            case LITERAL_TYPE_TEXT_BLOCK:  resolvedType = CajetaType::of("String"); break;
            case LITERAL_TYPE_NULL:        resolvedType = CajetaType::of("pointer"); break;
            case LITERAL_TYPE_CHAR:        resolvedType = CajetaType::of("char"); break;
            default: break;
        }
    }

    // Decode the inner content of a CHAR_LITERAL lexeme (the bit between the single
    // quotes, with escapes still raw). Returns the code-unit value; values outside
    // 0..255 are truncated by the caller's i8 cast.
    static int decodeCharLiteral(const string& inner) {
        if (inner.empty()) return 0;
        if (inner[0] != '\\') {
            // Plain byte. Higher bytes (UTF-8 multibyte) just take the first byte —
            // Cajeta `char` is int8 today, so multibyte source chars don't fit.
            return (unsigned char) inner[0];
        }
        if (inner.size() < 2) return 0;
        char c = inner[1];
        switch (c) {
            case 'b': return 0x08;
            case 't': return 0x09;
            case 'n': return 0x0A;
            case 'f': return 0x0C;
            case 'r': return 0x0D;
            case '"': return 0x22;
            case '\'': return 0x27;
            case '\\': return 0x5C;
            case 'u': {
                // Skip extra 'u's per Java spec, then read 4 hex digits.
                size_t i = 1;
                while (i < inner.size() && inner[i] == 'u') i++;
                int v = 0;
                int read = 0;
                while (i < inner.size() && read < 4) {
                    char h = inner[i++];
                    int d;
                    if (h >= '0' && h <= '9') d = h - '0';
                    else if (h >= 'a' && h <= 'f') d = 10 + (h - 'a');
                    else if (h >= 'A' && h <= 'F') d = 10 + (h - 'A');
                    else break;
                    v = (v << 4) | d;
                    read++;
                }
                return v;
            }
            default: {
                // Octal: up to 3 digits.
                if (c >= '0' && c <= '7') {
                    int v = 0;
                    size_t i = 1;
                    int read = 0;
                    while (i < inner.size() && read < 3
                            && inner[i] >= '0' && inner[i] <= '7') {
                        v = (v << 3) | (inner[i] - '0');
                        i++;
                        read++;
                    }
                    return v;
                }
                return (unsigned char) c;
            }
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

    // Decode backslash-escapes in a string-literal body (already stripped of its
    // surrounding quotes). Mirrors the lexer's EscapeSequence rule: \b \t \n \f \r
    // \" \' \\, octal \NNN (1–3 digits), and \uXXXX (one or more u's allowed).
    static string decodeStringLiteral(const string& src) {
        string out;
        out.reserve(src.size());
        size_t i = 0;
        while (i < src.size()) {
            char c = src[i++];
            if (c != '\\' || i >= src.size()) { out.push_back(c); continue; }
            char e = src[i++];
            switch (e) {
                case 'b': out.push_back('\b'); break;
                case 't': out.push_back('\t'); break;
                case 'n': out.push_back('\n'); break;
                case 'f': out.push_back('\f'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"');  break;
                case '\'': out.push_back('\''); break;
                case '\\': out.push_back('\\'); break;
                case 'u': {
                    while (i < src.size() && src[i] == 'u') i++;
                    int v = 0;
                    int read = 0;
                    while (i < src.size() && read < 4) {
                        char h = src[i];
                        int d;
                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = 10 + (h - 'a');
                        else if (h >= 'A' && h <= 'F') d = 10 + (h - 'A');
                        else break;
                        v = (v << 4) | d;
                        i++;
                        read++;
                    }
                    // For now we encode the code-point as a single byte if it fits;
                    // larger code points are dropped on the floor. Proper UTF-8
                    // encoding lands when String becomes UTF-8 end-to-end.
                    if (v <= 0xFF) out.push_back((char) v);
                    break;
                }
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0';
                        int read = 1;
                        while (i < src.size() && read < 3
                                && src[i] >= '0' && src[i] <= '7') {
                            v = (v << 3) | (src[i] - '0');
                            i++;
                            read++;
                        }
                        out.push_back((char) v);
                    } else {
                        out.push_back(e);
                    }
            }
        }
        return out;
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
                return module->getBuilder()->CreateGlobalStringPtr(
                    decodeStringLiteral(stripQuotes(value)), "str");
            case LITERAL_TYPE_CHAR: {
                // Strip the single-quote pair, then decode any escape into a byte value.
                string inner = value;
                if (inner.size() >= 2 && inner.front() == '\'' && inner.back() == '\'') {
                    inner = inner.substr(1, inner.size() - 2);
                }
                int v = decodeCharLiteral(inner);
                return llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), v, /*isSigned=*/true);
            }
            default:
                return nullptr;
        }
    }

    // True if the integer literal carries an `L` / `l` long-suffix.
    // The lexer permits `[lL]?` on every integer-literal form (decimal,
    // hex, oct, binary); presence of the suffix pins the value to int64.
    static bool hasLongSuffix(const string& s) {
        if (s.empty()) return false;
        char last = s.back();
        return last == 'l' || last == 'L';
    }

    void IntegerLiteralExpression::resolveTypes(CajetaModulePtr module) {
        // Long-suffix (`8L`) → int64; otherwise default to int32 and let
        // widening at boundaries (assignment, BinaryOpExpression, method-
        // arg coercion) promote when the context demands a wider type.
        resolvedType = CajetaType::of(hasLongSuffix(value) ? "int64" : "int32");
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
        size_t prefixLen = 0;
        switch (integerLiteralType) {
            case INTEGER_LITERAL_TYPE_BINARY: radix = 2;  prefixLen = 2; break;  // "0b" / "0B"
            case INTEGER_LITERAL_TYPE_OCT:    radix = 8;  prefixLen = 0; break;  // leading 0 is OK for APInt base-8
            case INTEGER_LITERAL_TYPE_HEX:    radix = 16; prefixLen = 2; break;  // "0x" / "0X"
            default:                          radix = 10; prefixLen = 0; break;
        }

        // Strip the radix prefix (APInt expects pure digits in the given base),
        // any digit-grouping underscores the lexer accepts (e.g. `1_000_000`),
        // and the optional trailing `L`/`l` long-suffix. Without the strip,
        // APInt(64, "8L", 10) misreads non-digit chars and `value` lands as
        // garbage — surfaced by the int64-literal path during method-template
        // testing.
        string numericText = value;
        if (prefixLen && numericText.size() >= prefixLen) {
            numericText.erase(0, prefixLen);
        }
        if (!numericText.empty()) {
            char last = numericText.back();
            if (last == 'l' || last == 'L') numericText.pop_back();
        }
        // Erase underscores in place.
        numericText.erase(
            std::remove(numericText.begin(), numericText.end(), '_'),
            numericText.end());

        // Default storage is 64-bit so any value parses correctly; the
        // ReturnStatement / BinaryOpExpression boundary code (and
        // CajetaType::normalize when we get to it) coerces to the surrounding
        // context's expected width. resolveTypes pins the Cajeta type to
        // int32 (or int64 for L-suffixed literals) for type inference.
        llvm::Type* valueType = llvm::IntegerType::getInt64Ty(*module->getLlvmContext());
        llvm::APInt apint(64, numericText, radix);
        return llvm::ConstantInt::get(valueType, apint);
    }
} // code