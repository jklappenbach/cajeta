//
// transform-intrinsics Unit 5 — Vmap (spec §6.1, §6.2). `Vmap(f)` lifts `f` over a
// leading batch axis as a compile-time transform over f's specialized body, using
// the same Tier-A source synthesis as Grad (U3): the forward DAG validates that
// every primitive in the body has a batching rule, then emitBatchedSource
// assembles the batched helper class.
//
// Batching a leading axis over the elementwise scalar primitives IS per-element
// application, so the lifted form is f's own body evaluated at each index. The
// registry is what keeps that honest: a primitive that does NOT distribute over
// the axis has no rule, and Vmap rejects it by name (§6.2) rather than emitting a
// batch that is quietly wrong.
//
#pragma once

#include <string>

namespace cajeta {
    namespace transform {

        // Whether `primitive` (a VjpRegistry/AdNode primitive id) has a v1
        // batching rule. The elementwise scalar set — add/sub/mul/negate — lifts
        // per element. The tensor primitives contract or reduce ACROSS axes
        // (`matmul`, `sum`), so a leading batch axis changes their meaning and
        // they need a real per-op rule; until that lands they are rejected by
        // name rather than silently mis-batched.
        bool hasBatchRule(const std::string& primitive);

        // Assemble the Tier-A batched helper-class source. The class holds one
        // static `make()` returning the batched function as a lambda over the
        // batch array, mirroring emitBackwardSource's shape so the recognizer
        // reuses the same parse-extract + codegen + call seam:
        //   static (T[]) -> #R[] make() {
        //       return (T[] __xs) -> {
        //           R[] __out = heap R[__xs.count()];
        //           for (int32 __i = 0; __i < __xs.count(); __i = __i + 1) {
        //               T <param> = __xs[__i];
        //               __out[__i] = <bodyExpr>;
        //           }
        //           return #__out;
        //       };
        //   }
        // `bodyExpr` is f's inlined forward source over `paramName`, so the batch
        // element type follows f's own result type.
        std::string emitBatchedSource(const std::string& className,
                                      const std::string& paramName,
                                      const std::string& paramTypeName,
                                      const std::string& resultTypeName,
                                      const std::string& bodyExpr,
                                      bool importGradResult = false);

    } // namespace transform
} // namespace cajeta
