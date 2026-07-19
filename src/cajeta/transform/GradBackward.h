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

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cajeta {
    class Expression;
    namespace transform {

        // U4 — how a call to a user function resolves for the DAG walk. The
        // resolver (supplied by the Grad recognizer, which has the enclosing
        // class) maps a call name+arity to either a differentiate-through inline
        // target (the callee's single return expression + its param names) or a
        // @NoGrad stop-gradient (the call becomes a constant leaf). `found` false
        // means "not a resolvable user helper" — the caller then errors as an
        // unsupported body. The resolver stays in the compiler core so this
        // transform lib carries no CajetaClass/Method dependency.
        struct InlineTarget {
            bool found = false;
            bool noGrad = false;             // @NoGrad -> constant leaf
            bool returnIsTensor = false;     // rank of the call's result
            std::string returnTy;            // callee return type canonical (value-type fallback)
            std::string qualifiedName;       // "G.sq" — forward-value source for @NoGrad
            std::vector<std::string> paramNames;   // callee params (inline case)
            Expression* body = nullptr;      // callee's single return expr (inline case)
        };
        using CallResolver =
            std::function<InlineTarget(const std::string& name, size_t arity)>;

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
            bool isTensor = false;          // rank tag: this node's value is a tensor
        };

        // Build f's forward DAG from the lambda body expression (nodes in
        // topological order, back() = output). Input-param leaves are deduped so a
        // reused input accumulates its cotangents. v1 supports `+ - *` and unary `-`
        // over scalar parameters, and `Tensor.mul/add/sub/matmul/sum` over tensor
        // parameters; any other construct returns false and sets *err. `paramIsTensor`
        // seeds the rank of each input leaf per parameter name (a `Tensor.<op>`
        // node's rank is structural: elementwise ops stay tensor, `sum` reduces to
        // scalar), so a multi-arg f can mix scalar and tensor parameters.
        bool buildDag(Expression* body,
                      const std::vector<std::string>& paramNames,
                      const std::map<std::string, bool>& paramIsTensor,
                      const CallResolver& resolveCall,
                      std::vector<AdNode>& outNodes,
                      std::map<std::string, size_t>& outParamNodeIndex,
                      std::string* err);

        // Reverse-mode over `nodes` (back() = output): seed the output cotangent to
        // 1.0f and compose VjpRegistry rules in reverse, returning the grad SOURCE
        // expression accumulated at `paramIndex`. A primitive with no registered
        // rule returns "" and sets *missingPrimitive to its id (the §5.3 signal).
        // `elem` is the tensor element-type spelling (e.g. "float32"); rules and
        // cotangent accumulation over tensor-tagged nodes are emitted over it.
        std::string reverseModeGrad(const std::vector<AdNode>& nodes,
                                    size_t paramIndex,
                                    const std::string& elem,
                                    std::string* missingPrimitive);

        // Assemble the Tier-A backward helper-class source. The class holds one
        // static `make()` that RETURNS the backward as a lambda taking ALL of f's
        // params (so the closure keeps f's exact arity) but grading only the
        // selected arg:
        //   static (P0,P1) -> GradResult<V,G> make() {
        //       return (P0 p0, P1 p1) -> stack GradResult<V,G>(outputValueExpr, gradExpr);
        //   }
        // Returning a lambda reuses the existing closure-record + value-type-sret
        // machinery — `Grad(f)` recognizer just parse-extracts `make`, codegens it,
        // and emits a call to it to obtain the closure value.
        std::string emitBackwardSource(const std::string& className,
                                       const std::vector<std::string>& paramNames,
                                       const std::vector<std::string>& paramTypeNames,
                                       const std::string& valueTypeName,
                                       const std::string& gradTypeName,
                                       const std::string& outputValueExpr,
                                       const std::string& gradExpr,
                                       bool importTensor = false);

    } // namespace transform
} // namespace cajeta
