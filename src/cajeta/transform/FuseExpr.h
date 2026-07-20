//
// nucleo-expr Unit 1 — compile-time fused tensor expressions (spec §2, §3, §5).
//
// `Fuse(f)` takes an elementwise tensor expression and emits ONE loop over the
// element index, with every operator inlined into the loop body as SCALAR
// arithmetic. No intermediate tensors are allocated: the chain reads each input
// element once and writes each output element once, which is the spec's
// headline claim (§3.1/§3.2) and the answer to NumPy's temporary-per-operator.
//
// The mechanism is the Tier-A one already proven by Grad/Vmap/Jit: recognize
// the intrinsic, walk the body into the shared `AdNode` DAG, synthesize source,
// codegen it. The difference is WHAT is synthesized — Grad emits the backward,
// Vmap a batch loop; Fuse emits the element loop with tensor-level calls
// lowered to scalar ops.
//
#pragma once

#include <string>
#include <vector>

namespace cajeta {
    namespace transform {

        struct AdNode;

        // Whether `primitive` fuses elementwise — i.e. its element-level form is
        // scalar arithmetic on the operands' element-level forms. Reductions
        // (`sum`/`mean`) do NOT: they bound a fusion region (U2).
        bool fusesElementwise(const std::string& primitive);

        // The ELEMENT-level source of node `idx` — the whole subexpression as
        // scalar arithmetic over `paramName.get1(<indexVar>)` leaves. This is
        // what collapses N tensor operators into one loop body with no
        // temporaries. Returns "" and sets *err when the node (or any operand)
        // is not elementwise-fusible.
        std::string elementExpr(const std::vector<AdNode>& nodes, size_t idx,
                                const std::string& indexVar, std::string* err);

        // Assemble the fused helper-class source: one static make() returning a
        // lambda that allocates the single result tensor and runs the fused
        // loop, mirroring emitBackwardSource/emitBatchedSource so the recognizer
        // reuses the same parse-extract + codegen + call seam.
        //
        //   static (Tensor<E>) -> #Tensor<E> make() {
        //       return (Tensor<E> p) -> {
        //           int64 n = p.size();
        //           Tensor<E> out = Tensor.zerosLike<E>(p);
        //           for (int64 i = 0; i < n; i = i + 1) { out.set1(i, <elemExpr>); }
        //           return #out;
        //       };
        //   }
        std::string emitFusedSource(const std::string& className,
                                    const std::string& paramName,
                                    const std::string& elem,
                                    const std::string& elemExprSrc);

    } // namespace transform
} // namespace cajeta
