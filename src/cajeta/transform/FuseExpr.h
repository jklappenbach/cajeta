// Compile-time fusion of an elementwise tensor expression into one scalar loop
// over the element index, synthesized as cajeta source and codegen'd.
#pragma once

#include <string>
#include <vector>

namespace cajeta {
    namespace transform {

        struct AdNode;

        // Whether `primitive`'s element-level form is scalar arithmetic on its
        // operands' element forms. Reductions do not: they bound a fusion region.
        bool fusesElementwise(const std::string& primitive);

        // A reduction hoisted out of the loop: `local` is the preheader variable
        // name, `init` its tensor-level initializer source.
        struct Hoist {
            std::string local;
            std::string init;
        };

        // Element-level source of node `idx`: the subexpression as scalar arithmetic
        // over `paramName.get1(<indexVar>)` leaves, loop-invariant reductions staged
        // into `hoists`; "" and *err when not fusible. A root reduction is not a loop.
        std::string elementExpr(const std::vector<AdNode>& nodes, size_t idx,
                                const std::string& indexVar,
                                std::vector<Hoist>* hoists, std::string* err);

        // Whether `primitive` reduces a tensor to a scalar, bounding a fusion
        // region rather than fusing into it.
        bool isReduction(const std::string& primitive);

        // Assemble the fused helper-class source: one static make() returning a
        // lambda that allocates the result tensor and runs the fused loop. Shaped
        // like emitBackwardSource so the recognizer reuses its codegen seam.
        std::string emitFusedSource(const std::string& className,
                                    const std::string& paramName,
                                    const std::string& elem,
                                    const std::string& elemExprSrc,
                                    const std::vector<Hoist>& hoists);

        // The scalar-valued fused form for a reduction-rooted expression: the
        // elementwise body fuses into the accumulation loop, with no output tensor.
        std::string emitFusedReductionSource(const std::string& className,
                                             const std::string& paramName,
                                             const std::string& elem,
                                             const std::string& reductionPrim,
                                             const std::string& elemExprSrc,
                                             const std::vector<Hoist>& hoists);

    } // namespace transform
} // namespace cajeta
