//
// Created by James Klappenbach on 11/6/22.
//

#include "LiteralUtils.h"
#include <string>

using namespace std;

namespace cajeta {

    __int128_t LiteralUtils::octalToInt128(string& value, int bits) {
        __int128_t res = 0;
        size_t i = 0;
        bool sign = false;

        if (value[i] == '-') {
            ++i;
            sign = true;
        }

        if (value[i] == '+') {
            ++i;
        }

        while (i < value.size()) {
            const char c = value[i];
            if (c >= '0' && c <= '7') {
                res *= 8;
                res += c - '0';
                i++;
            } else {
                throw string("Non octal digit: ") + c;
            }
        }

        if (sign) {
            res *= -1;
        }

        return res;
    }

    __int128_t LiteralUtils::hexToInt128(string& value, int bits) {
        __int128_t res = 0;
        size_t i = 0;
        bool sign = false;

        if (value[i] == '-') {
            ++i;
            sign = true;
        }

        if (value[i] == '+') {
            ++i;
        }

        while (i < value.size()) {
            const char c = value[i];
            if (std::isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
                res *= 16;
                if (std::isdigit(c)) {
                    res += c - '0';
                } else if (c >= 'A' && c <= 'F') {
                    res += c - 'A';
                } else {
                    res += c - 'a';
                }
                i++;
            } else {
                throw string("Non-hex digit: ") + c;
            }
        }

        if (sign) {
            res *= -1;
        }

        return res;
    }

    __int128_t LiteralUtils::binaryToInt128(string& value, int bits) {
        __int128_t res = 0;
        size_t i = 0;
        bool sign = false;

        if (value[i] == '-') {
            ++i;
            sign = true;
        }

        if (value[i] == '+') {
            ++i;
        }

        while (i < value.size()) {
            if (i > bits) {
                throw string("Literal is too large for the assigned data type");
            }
            const char c = value[i];
            if (not std::isdigit(c)) {
                throw std::string("Non-numeric character: ") + c;
            } else if (c != '0' && c != '1') {
                throw string("Non-binary digit: ") + c;
            }
            res *= 2;
            res += c - '0';
            i++;
        }

        if (sign) {
            res *= -1;
        }

        return res;
    }

    __int128_t LiteralUtils::decimalToInt128(string& value, int bits) {
        __int128_t res = 0;
        size_t i = 0;
        bool sign = false;

        if (value[i] == '-') {
            ++i;
            sign = true;
        }

        if (value[i] == '+') {
            ++i;
        }

        while (i < value.size()) {
            const char c = value[i];
            if (not std::isdigit(c)) {
                throw std::string("Non-numeric character: ") + c;
            }
            res *= 10;
            res += c - '0';
            i++;
        }

        if (sign) {
            res *= -1;
        }

        return res;
    }

//    llvm::APInt int128ToType(__int128_t value, unsigned bits, bool isSigned) {
//        uint64_t first;
//        uint64_t second;
//
//        switch (bits) {
//            case 8:
//            case 16:
//            case 32:
//            case 64:
//                return llvm::APInt(bits, (uint64_t) value, isSigned);
//            case 128:
//                first = value | 0xFFFFFFFFFFFFFFFF;
//                value >> 64;
//                second = value | 0xFFFFFFFFFFFFFFFF;
//                return llvm::APInt(bits, { first, second }, isSigned);
//
//
//        }
//    }

}