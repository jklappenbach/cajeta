// ConstExpr — integer constant expressions over named compile-time values
// (xpu-kernel-independence §3.3). A class template's non-type parameters
// size arrays and fill annotation arguments, `[WM / 16]` and
// `@Occupancy(maxWaves = (TM / WM) * (TN / WN))`, and both arrive as source
// text. This evaluates that text per instantiation.
//
// The grammar is the integer subset of the language: decimal and hex
// literals (with `_` grouping and an `L` suffix), names, parentheses, a
// primitive cast such as `(int64)`, unary `- + ~`, and the binary operators
// `* / % + - << >> & ^ |` at the language's precedence.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace cajeta {
    struct ConstExprResult {
        bool ok = false;
        int64_t value = 0;
        // When !ok: the first name the lookup could not bind, else empty.
        std::string unbound;
        // When !ok: what went wrong, for a diagnostic.
        std::string error;
    };

    using ConstLookup = std::function<std::optional<int64_t>(const std::string&)>;

    ConstExprResult evalConstExpr(const std::string& text, const ConstLookup& lookup);
}
