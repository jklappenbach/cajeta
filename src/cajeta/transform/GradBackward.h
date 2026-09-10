// Reverse-mode autodiff: a forward DAG built from f's AST, VJP rules composed in
// reverse, and a backward helper class emitted as Tier-A cajeta source.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cajeta {
    class Expression;
    namespace transform {

        // How a call to a user function resolves for the DAG walk: an inline
        // target, a @NoGrad constant leaf, or `found` false, which is an error.
        struct InlineTarget {
            bool found = false;
            bool noGrad = false;             // @NoGrad -> constant leaf
            bool returnIsTensor = false;     // rank of the call's result
            std::string returnTy;            // callee return type canonical (value-type fallback)
            std::string qualifiedName;       // "G.sq" — forward-value source for @NoGrad
            std::vector<std::string> paramNames;   // callee params (inline case)
            Expression* body = nullptr;      // callee's single return expr (inline case)
        };
        // `recv` is the written receiver ("" for a bare same-class call).
        using CallResolver =
            std::function<InlineTarget(const std::string& recv,
                                       const std::string& name, size_t arity)>;

        // A node in f's forward DAG, topologically ordered with back() the output;
        // `valueExpr` is a leaf's name/literal or a primitive's inlined forward source.
        struct AdNode {
            std::string valueExpr;
            bool isInputParam = false;
            std::string primitive;          // "" for a leaf
            std::vector<size_t> operands;   // child node indices (primitive only)
            bool isTensor = false;          // rank tag: this node's value is a tensor
        };

        // Build f's forward DAG from the lambda body, false and *err on any
        // unsupported construct. Input-param leaves are DEDUPED so a reused input
        // accumulates its cotangents; `paramIsTensor` seeds each leaf's rank.
        bool buildDag(Expression* body,
                      const std::vector<std::string>& paramNames,
                      const std::map<std::string, bool>& paramIsTensor,
                      const CallResolver& resolveCall,
                      std::vector<AdNode>& outNodes,
                      std::map<std::string, size_t>& outParamNodeIndex,
                      std::string* err);

        // Compose VjpRegistry rules in reverse from a seed cotangent of 1.0f,
        // returning the grad source expression accumulated at `paramIndex` and
        // emitted over element type `elem`; "" and *missingPrimitive if unruled.
        std::string reverseModeGrad(const std::vector<AdNode>& nodes,
                                    size_t paramIndex,
                                    const std::string& elem,
                                    std::string* missingPrimitive);

        // Assemble the backward helper-class source: one static `make()` returning
        // the backward as a lambda over ALL of f's params (keeping its arity) but
        // grading only the selected arg, so existing closure codegen is reused.
        std::string emitBackwardSource(const std::string& className,
                                       const std::vector<std::string>& paramNames,
                                       const std::vector<std::string>& paramTypeNames,
                                       const std::string& valueTypeName,
                                       const std::string& gradTypeName,
                                       const std::string& outputValueExpr,
                                       const std::string& gradExpr,
                                       bool importTensor = false);

        // The GradAll<K> variant: one closure returning GradResult<V, GT[]> for the
        // leading K args; the array is built by a sibling static, as the body is one expression.
        std::string emitGradAllSource(const std::string& className,
                                      const std::vector<std::string>& paramNames,
                                      const std::vector<std::string>& paramTypeNames,
                                      const std::string& valueTypeName,
                                      const std::string& gradTypeName,
                                      const std::string& outputValueExpr,
                                      const std::vector<std::string>& gradExprs,
                                      bool importTensor);

    } // namespace transform
} // namespace cajeta
