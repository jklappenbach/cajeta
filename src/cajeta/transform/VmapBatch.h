// `Vmap(f)` lifts `f` over a leading batch axis by source synthesis: every
// primitive in the body must have a batching rule, and one that does not
// distribute over the axis is rejected by name rather than quietly mis-batched.
#pragma once

#include <string>

namespace cajeta {
    namespace transform {

        // Whether `primitive` has a batching rule. The elementwise scalars do;
        // the tensor primitives contract or reduce across axes, so they do not.
        bool hasBatchRule(const std::string& primitive);

        // Assemble the batched helper-class source: one static `make()` returning a
        // lambda that walks the batch array and evaluates `bodyExpr`, f's inlined
        // forward source over `paramName`, at each index. Mirrors emitBackwardSource.
        std::string emitBatchedSource(const std::string& className,
                                      const std::string& paramName,
                                      const std::string& paramTypeName,
                                      const std::string& resultTypeName,
                                      const std::string& bodyExpr,
                                      bool importGradResult = false);

    } // namespace transform
} // namespace cajeta
