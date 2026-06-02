//
// CajetaConstantType — a compile-time integer constant standing in as a
// NON-TYPE template argument (the `N` in `Vector<T, N>`).
//
// It is NOT a real value type: getLlvmType() is null and it is never used as
// a field/local/parameter slot. It only ever rides inside a template-argument
// list, carrying its integer through the existing `vector<CajetaTypePtr>` +
// substitution machinery with zero signature churn. Its canonical name is the
// decimal text of the value, so `buildArgSuffix` keys instantiations of the
// same template with different N distinctly (e.g. `Vector<float32,3>` vs
// `Vector<float32,4>`).
//

#pragma once

#include "CajetaType.h"

namespace cajeta {
    class CajetaConstantType : public CajetaType {
    private:
        int64_t value;
    public:
        explicit CajetaConstantType(int64_t value);

        int64_t getValue() const { return value; }

        // Parse a `Vector<..., N>`-style integer-literal node to an int64,
        // honoring decimal/hex/octal/binary, digit-grouping underscores, and
        // an optional trailing `L`/`l` (mirrors IntegerLiteralExpression's
        // radix handling).
        static int64_t parseLiteral(CajetaParser::IntegerLiteralContext* ctx);

        static CajetaTypePtr of(int64_t value);
    };
    typedef shared_ptr<CajetaConstantType> CajetaConstantTypePtr;
}
