//
// CirBuilder — lowers a type-resolved Method's AST into a CirFunction
// (spec §2, §3.1.1). Phase A scope: ENOUGH for the closed slice (a generic
// function with a function-typed parameter, its callers, and the closures
// passed) — params, locals, arithmetic/compare, array index, calls,
// lambda -> make.closure, closure-param call -> apply.closure, and the
// surrounding control flow walked for closure discovery. Out-of-slice
// constructs are represented opaquely: never a crash, never a false "known"
// (analysis-only — gaps fall back to LEAVE-INDIRECT, never miscompile).
//
// The lowering is intentionally a single basic block (the Phase-A closure
// analysis is structural / cross-call, not intra-CFG dataflow); a faithful
// CFG is Phase B. The key invariants it DOES preserve: a directly-supplied
// non-capturing lambda lowers to `make.closure(<fn>, ∅)` with target-known,
// and a call through a function-typed value lowers to `apply.closure`.
//

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
