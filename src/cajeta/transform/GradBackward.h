//
// transform-intrinsics Unit 3 — reverse-mode autodiff over a forward DAG, emitted
// as Tier-A cajeta source (spec §5, §8.1). `Grad(f)` builds the DAG from f's AST,
// calls reverseModeGrad to compose the VJP rules (VjpRegistry) in reverse into a
// grad source expression, and emitBackwardSource assembles the backward helper
// class that returns `GradResult<V,G>{value, grads}`.
//
// The forward source of each subexpression is INLINED (the ops are pure), so no
// SSA temporaries are needed: `Grad((float32 x) -> x*x)` synthesizes
// `return stack GradResult<float32,float32>(x * x, (1.0f) * x + (1.0f) * x);`.
//
#pragma once

#include <string>
#include <vector>

namespace cajeta {
    namespace transform {

        // A node in f's forward computation DAG. Nodes are in topological order;
        // the last node is the output. A leaf carries `valueExpr` = the input
        // parameter name (isInputParam) or a constant literal; a primitive carries
        // the VJP primitive id and indices of its operand nodes, and `valueExpr` =
        // the inlined forward source of the whole subexpression.
        struct AdNode {
            std::string valueExpr;
            bool isInputParam = false;
            std::string primitive;          // "" for a leaf
            std::vector<size_t> operands;   // child node indices (primitive only)
        };

        // Reverse-mode over `nodes` (back() = output): seed the output cotangent to
        // 1.0f and compose VjpRegistry rules in reverse, returning the grad SOURCE
        // expression accumulated at `paramIndex`. A primitive with no registered
        // rule returns "" and sets *missingPrimitive to its id (the §5.3 signal).
        std::string reverseModeGrad(const std::vector<AdNode>& nodes,
                                    size_t paramIndex,
                                    std::string* missingPrimitive);

        // Assemble the Tier-A backward helper-class source. The class holds one
        // static `make()` that RETURNS the backward as a lambda:
        //   static (P) -> GradResult<V,G> make() {
        //       return (P p) -> stack GradResult<V,G>(outputValueExpr, gradExpr);
        //   }
        // Returning a lambda reuses the existing closure-record + value-type-sret
        // machinery — `Grad(f)` recognizer just parse-extracts `make`, codegens it,
        // and emits a call to it to obtain the closure value.
        std::string emitBackwardSource(const std::string& className,
                                       const std::string& paramName,
                                       const std::string& paramTypeName,
                                       const std::string& valueTypeName,
                                       const std::string& gradTypeName,
                                       const std::string& outputValueExpr,
                                       const std::string& gradExpr);

    } // namespace transform
} // namespace cajeta
