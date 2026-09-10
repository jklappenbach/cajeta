// LiteralUtils - integer literal text to 128-bit values and llvm::APInt.

#pragma once

#include <string>
#include <llvm/ADT/APInt.h>

using namespace std;

namespace cajeta {
    class LiteralUtils {
    public:
        // `value` is RAW token text: optional sign, `0b` prefix, `_` grouping, `l`/`L`
        // suffix. Throws CAJETA_ERROR_MALFORMED_INT_LITERAL on a non-binary digit or on
        // more than `bits` digits — the only converter here that enforces a width.
        static __int128_t binaryToInt128(string& value, int bits);

        // Raw token text, no radix prefix; `bits` is unused, so nothing is
        // width-checked. Throws CAJETA_ERROR_MALFORMED_INT_LITERAL on a non-decimal digit.
        static __int128_t decimalToInt128(string& value, int bits);

        // Raw token text with an optional `0x`/`0X` prefix; `bits` is unused.
        // Throws CAJETA_ERROR_MALFORMED_INT_LITERAL on a non-hex digit.
        static __int128_t hexToInt128(string& value, int bits);

        // Raw token text, digits 0-7 with NO radix prefix; `bits` is unused.
        // Throws CAJETA_ERROR_MALFORMED_INT_LITERAL on a non-octal digit.
        static __int128_t octalToInt128(string& value, int bits);

        static llvm::APInt int128ToType(__int128_t value, int bits);
    };
}