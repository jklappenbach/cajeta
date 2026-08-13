//
// Created by James Klappenbach on 11/6/22.
//

#include "LiteralUtils.h"
#include "../error/Exception.h"
#include <cctype>
#include <string>

using namespace std;

namespace cajeta {

    // The converters take the grammar's RAW token text: an optional sign, an
    // optional radix prefix (0x / 0b; octal's is its leading 0), digit-group
    // underscores, and an optional l/L suffix (DECIMAL_LITERAL etc. in
    // CajetaLexer.g4). Each strips that dressing itself, so callers can pass
    // getRawValue() straight through. A character the lexer should have made
    // impossible throws a catchable Exception, never a raw std::string.
    namespace {

    struct LiteralScan {
        bool sign = false;
        size_t i = 0;       // first digit
        size_t end = 0;     // one past the last digit (suffix removed)
    };

    LiteralScan scanDressing(const string& value, const char* prefixChars) {
        LiteralScan s;
        s.end = value.size();
        if (s.i < s.end && (value[s.i] == '-' || value[s.i] == '+')) {
            s.sign = value[s.i] == '-';
            ++s.i;
        }
        if (prefixChars && s.i + 1 < s.end && value[s.i] == '0'
                && (value[s.i + 1] == prefixChars[0]
                    || value[s.i + 1] == prefixChars[1])) {
            s.i += 2;
        }
        if (s.end > s.i && (value[s.end - 1] == 'l' || value[s.end - 1] == 'L')) {
            --s.end;
        }
        return s;
    }

    [[noreturn]] void badDigit(const char* radixName, char c) {
        throw Exception(string("malformed integer literal: non-") + radixName
                            + " digit '" + c + "'",
                        "CAJETA_ERROR_MALFORMED_INT_LITERAL");
    }

    } // namespace

    __int128_t LiteralUtils::octalToInt128(string& value, int bits) {
        LiteralScan s = scanDressing(value, nullptr);
        __int128_t res = 0;
        for (size_t i = s.i; i < s.end; ++i) {
            const char c = value[i];
            if (c == '_') continue;
            if (c < '0' || c > '7') badDigit("octal", c);
            res = res * 8 + (c - '0');
        }
        return s.sign ? -res : res;
    }

    __int128_t LiteralUtils::hexToInt128(string& value, int bits) {
        LiteralScan s = scanDressing(value, "xX");
        __int128_t res = 0;
        for (size_t i = s.i; i < s.end; ++i) {
            const char c = value[i];
            if (c == '_') continue;
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else badDigit("hex", c);
            res = res * 16 + digit;
        }
        return s.sign ? -res : res;
    }

    __int128_t LiteralUtils::binaryToInt128(string& value, int bits) {
        LiteralScan s = scanDressing(value, "bB");
        __int128_t res = 0;
        int digits = 0;
        for (size_t i = s.i; i < s.end; ++i) {
            const char c = value[i];
            if (c == '_') continue;
            if (c != '0' && c != '1') badDigit("binary", c);
            if (++digits > bits) {
                throw Exception(
                    "binary literal has more than " + std::to_string(bits)
                        + " digits",
                    "CAJETA_ERROR_MALFORMED_INT_LITERAL");
            }
            res = res * 2 + (c - '0');
        }
        return s.sign ? -res : res;
    }

    __int128_t LiteralUtils::decimalToInt128(string& value, int bits) {
        LiteralScan s = scanDressing(value, nullptr);
        __int128_t res = 0;
        for (size_t i = s.i; i < s.end; ++i) {
            const char c = value[i];
            if (c == '_') continue;
            if (!std::isdigit(static_cast<unsigned char>(c))) badDigit("decimal", c);
            res = res * 10 + (c - '0');
        }
        return s.sign ? -res : res;
    }

}
