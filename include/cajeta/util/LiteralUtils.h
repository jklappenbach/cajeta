//
// Created by James Klappenbach on 11/6/22.
//

#pragma once

#include <string>
#include <llvm/ADT/APInt.h>

using namespace std;

namespace cajeta {
    class LiteralUtils {
    public:
        static __int128_t binaryToInt128(string& value, int bits);
        static __int128_t decimalToInt128(string& value, int bits);
        static __int128_t hexToInt128(string& value, int bits);
        static __int128_t octalToInt128(string& value, int bits);
        static llvm::APInt int128ToType(__int128_t value, int bits);
    };
}