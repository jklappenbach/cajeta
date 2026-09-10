// CirBuilder — lowers a type-resolved Method's AST into a CirFunction (spec §2,
// §3.1.1). Out-of-slice constructs are represented opaquely, never as a false
// "known"; the lowering is one basic block, as Phase-A analysis is structural.

#pragma once

#include "Cir.h"

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace ir {

    class CirBuilder {
    public:
        // Lower a resolved method to CIR. Returns null only for a null method.
        static CirFunctionPtr buildFunction(const MethodPtr& method);
    };

} // namespace ir
} // namespace cajeta
