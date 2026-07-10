//
// Created by James Klappenbach on 4/14/23.
//

#include "LiteralExpression.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"
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
    // quotes, with escapes still raw). Returns the **Unicode codepoint** as an int —
    // up to 0x10FFFF. Source bytes are UTF-8; multibyte sequences decode to their
    // codepoint. The compiler emits the result as an i32 constant (char = i32 since
    // the 2026-05-18 redefinition); see CajetaType.cpp.
    static int decodeCharLiteral(const string& inner) {
        if (inner.empty()) return 0;
        if (inner[0] != '\\') {
            // Plain character. Single ASCII byte is the common case;
            // multibyte UTF-8 sequences (`'é'` = 0xC3 0xA9, `'😀'` =
            // 0xF0 0x9F 0x98 0x80) decode to the Unicode codepoint.
            unsigned char b0 = (unsigned char) inner[0];
            if (b0 < 0x80) {
                return b0;                       // ASCII fast path
            }
            // UTF-8 decode. Determine sequence length from the leading
            // byte. Malformed sequences fall back to returning the
            // first byte (caller sees a low-value codepoint rather
            // than a parse error — char-literal parse errors are out
            // of scope for v1).
            int seqLen = 0;
            int cp = 0;
            if ((b0 & 0xE0) == 0xC0) { seqLen = 2; cp = b0 & 0x1F; }
            else if ((b0 & 0xF0) == 0xE0) { seqLen = 3; cp = b0 & 0x0F; }
            else if ((b0 & 0xF8) == 0xF0) { seqLen = 4; cp = b0 & 0x07; }
            else return b0;
            if (inner.size() < (size_t) seqLen) return b0;
            for (int i = 1; i < seqLen; i++) {
                unsigned char b = (unsigned char) inner[i];
                if ((b & 0xC0) != 0x80) return b0;
                cp = (cp << 6) | (b & 0x3F);
            }
            return cp;
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
            case LITERAL_TYPE_TEXT_BLOCK: {
                // Phase 2b-β: a string literal materializes as a static
                // view-mode `cajeta.lang.String` instance, not a bare i8*
                // C-string. The byte payload is a private constant
                // `{ i64 count, [N+1 x i8] data }` (CajetaArray-shaped,
                // null-terminated for any legacy strlen reader); the
                // instance is a private mutable global whose layout
                // matches the class's struct (vtable, bytes, byteLength,
                // mode=1 view, cachedCpLength=-1 uncomputed). Mutable
                // because `count()` writes back its cached codepoint
                // count on first call. Each literal has its own static
                // pair; identical literals don't dedupe in v1 (LLVM may
                // still merge them via mergefunc/constmerge passes).
                //
                // Bootstrap fallback: until class String is registered,
                // emit the legacy i8* form. This lets the runtime
                // (including String.cajeta itself) compile cleanly; user
                // code always sees the class String materialization.
                std::string decoded = decodeStringLiteral(stripQuotes(value));
                CajetaTypePtr stringTy = CajetaType::of("String");
                auto klass = std::dynamic_pointer_cast<CajetaClass>(stringTy);
                if (!klass || !klass->getLlvmType()
                        || !llvm::isa<llvm::StructType>(klass->getLlvmType())) {
                    return module->getBuilder()->CreateGlobalString(decoded, "str");
                }
                auto* structTy = llvm::cast<llvm::StructType>(klass->getLlvmType());
                // Emit the string-literal globals into the module the builder is
                // currently emitting into (the emit/user module for a reuse-path
                // instantiation), not the resolution module. == getLlvmModule()
                // in production. The vtable cross-module fixup below is keyed off
                // the same `mod`.
                auto* mod = module->emitTargetLlvmModule();
                auto* i8Ty = llvm::Type::getInt8Ty(ctx);
                auto* i32Ty = llvm::Type::getInt32Ty(ctx);
                auto* i64Ty = llvm::Type::getInt64Ty(ctx);
                auto* ptrTy = llvm::PointerType::get(ctx, 0);
                int64_t len = (int64_t) decoded.size();

                // 6.2.2 tagged core: {vtable, lenTag, aux, base, ccp}.
                //   len <= 12  Inline — the text bytes pack into the aux
                //              i32 (bytes 0..3, little-endian) and the
                //              base ptr slot (bytes 4..11 as an inttoptr
                //              i64 constant); no byte global at all.
                //   len >  12  Static pointer form — lenTag carries the
                //              STATIC bit (1<<29), aux = 0 (window
                //              offset), base = the CajetaArray-shaped
                //              byte global (count word + text + NUL).
                // The instance global stays mutable because count()
                // writes back its cached codepoint count on first call.
                llvm::Constant* vtableRef =
                    llvm::ConstantPointerNull::get(ptrTy);
                if (auto* vt = klass->getVirtualTableGlobal()) {
                    vtableRef = CajetaModule::ensureGlobalInModule(mod, vt);
                }

                llvm::Constant* lenTagC;
                llvm::Constant* auxC;
                llvm::Constant* baseC;
                if (len <= 12) {
                    uint8_t ibuf[12] = {0};
                    memcpy(ibuf, decoded.data(), (size_t) len);
                    uint32_t auxBits = 0;
                    uint64_t baseBits = 0;
                    memcpy(&auxBits, ibuf, 4);
                    memcpy(&baseBits, ibuf + 4, 8);
                    lenTagC = llvm::ConstantInt::get(i32Ty,
                        llvm::APInt(32, (uint64_t) len, true));
                    auxC = llvm::ConstantInt::get(i32Ty,
                        llvm::APInt(32, (uint64_t) auxBits, false));
                    baseC = llvm::ConstantExpr::getIntToPtr(
                        llvm::ConstantInt::get(i64Ty,
                            llvm::APInt(64, baseBits, false)),
                        ptrTy);
                } else {
                    auto* dataInit = llvm::ConstantDataArray::getString(
                        ctx, decoded, /*addNull=*/true);
                    auto* arrStructTy = llvm::StructType::get(ctx,
                        llvm::ArrayRef<llvm::Type*>{i64Ty, dataInit->getType()});
                    auto* arrInit = llvm::ConstantStruct::get(arrStructTy,
                        llvm::ArrayRef<llvm::Constant*>{
                            llvm::ConstantInt::get(i64Ty,
                                llvm::APInt(64, (uint64_t) len, false)),
                            dataInit,
                        });
                    auto* bytesGv = new llvm::GlobalVariable(
                        *mod, arrStructTy, /*isConst=*/true,
                        llvm::GlobalValue::PrivateLinkage, arrInit, ".str.bytes");
                    lenTagC = llvm::ConstantInt::get(i32Ty,
                        llvm::APInt(32,
                            (uint64_t) (len | ((int64_t) 1 << 29)), true));
                    auxC = llvm::ConstantInt::get(i32Ty, 0);
                    baseC = bytesGv;
                }

                std::vector<llvm::Constant*> fields = {
                    vtableRef,
                    lenTagC,
                    auxC,
                    baseC,
                    llvm::ConstantInt::get(i32Ty,
                        llvm::APInt(32, (uint64_t) -1, true)),
                };
                for (unsigned fi = (unsigned) fields.size();
                        fi < structTy->getNumElements(); ++fi) {
                    fields.push_back(
                        llvm::Constant::getNullValue(structTy->getElementType(fi)));
                }
                auto* instInit = llvm::ConstantStruct::get(structTy,
                    llvm::ArrayRef<llvm::Constant*>(fields));
                auto* instGv = new llvm::GlobalVariable(
                    *mod, structTy, /*isConst=*/false,
                    llvm::GlobalValue::PrivateLinkage, instInit, ".str.inst");
                return instGv;
            }
            case LITERAL_TYPE_CHAR: {
                // Strip the single-quote pair, then decode the literal
                // into a Unicode codepoint (UTF-8 source bytes decoded
                // to a codepoint; \uXXXX escapes decoded as written).
                // Emitted as an i32 constant because Cajeta's `char`
                // is the codepoint type (since the 2026-05-18
                // redefinition). See CajetaType.cpp ~line 90.
                string inner = value;
                if (inner.size() >= 2 && inner.front() == '\'' && inner.back() == '\'') {
                    inner = inner.substr(1, inner.size() - 2);
                }
                int v = decodeCharLiteral(inner);
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), v, /*isSigned=*/true);
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