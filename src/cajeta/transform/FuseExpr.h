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

        // U2 — a reduction hoisted out of the loop. `local` is the preheader
        // variable name; `init` is its tensor-level initializer source.
        struct Hoist {
            std::string local;
            std::string init;
        };

        // The ELEMENT-level source of node `idx` — the whole subexpression as
        // scalar arithmetic over `paramName.get1(<indexVar>)` leaves. This is
        // what collapses N tensor operators into one loop body with no
        // temporaries. Returns "" and sets *err when the node (or any operand)
        // is not elementwise-fusible.
        //
        // A REDUCTION operand (sum/mean/std) does not fuse elementwise — it is
        // loop-invariant, so it is STAGED: appended to `hoists` and referenced
        // by its local name inside the loop (spec §3.3, "fuse what is legally
        // fusible and schedule the reduction as its own stage"). A reduction at
        // the ROOT is the whole expression's value and is not a loop at all —
        // the caller checks that case before asking for an element expression.
        //
        // THE COLUMN SEAM (plan X7-seam): this function is the ONLY place that
        // assumes a dense, always-valid buffer — the `get1(i)` leaf. A nullable
        // column needs validity propagation there and nowhere else; keep that
        // assumption from leaking into the emitter or the recognizer so a
        // null-aware variant slots in beside this rather than forcing a rewrite.
        // The contract is PINNED by test/nucleo/FuseSeamTests.cpp, which drives
        // this function directly over a hand-built DAG (exact element source,
        // reduction staging, get1-free emitters) — a null-aware variant must
        // match that shape, changing only the leaf read.
        std::string elementExpr(const std::vector<AdNode>& nodes, size_t idx,
                                const std::string& indexVar,
                                std::vector<Hoist>* hoists, std::string* err);

        // Whether `primitive` reduces a tensor to a scalar (bounds a fusion
        // region rather than fusing into it).
        bool isReduction(const std::string& primitive);

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
                                    const std::string& elemExprSrc,
                                    const std::vector<Hoist>& hoists);

        // A reduction-rooted expression (`sum(t*t)`) is a scalar-valued fused
        // form: the elementwise body fuses INTO the reduction's accumulation
        // loop, so there is no output tensor at all.
        std::string emitFusedReductionSource(const std::string& className,
                                             const std::string& paramName,
                                             const std::string& elem,
                                             const std::string& reductionPrim,
                                             const std::string& elemExprSrc,
                                             const std::vector<Hoist>& hoists);

    } // namespace transform
} // namespace cajeta
