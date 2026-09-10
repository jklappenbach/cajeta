// CajetaConstantType — a compile-time integer constant standing in as a NON-TYPE
// template argument (the `N` in `Vector<T, N>`). Not a real value type: getLlvmType()
// is null, and its canonical name is the decimal text, so arg suffixes key on N.

#pragma once

#include "CajetaType.h"

namespace cajeta {
    class CajetaConstantType : public CajetaType {
    private:
        int64_t value;
    public:
        explicit CajetaConstantType(int64_t value);

        int64_t getValue() const { return value; }

        // Parses an integer-literal node to int64 — decimal/hex/octal/binary, digit
        // grouping and a trailing `L`, mirroring IntegerLiteralExpression's radix handling.
        static int64_t parseLiteral(CajetaParser::IntegerLiteralContext* ctx);

        static CajetaTypePtr of(int64_t value);
    };
    typedef shared_ptr<CajetaConstantType> CajetaConstantTypePtr;
}
