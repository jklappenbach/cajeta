//
// transform-intrinsics Unit 3 — reverse-mode autodiff + Tier-A backward emission.
//
#include "cajeta/transform/GradBackward.h"
#include "cajeta/transform/VjpRegistry.h"

#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/asn/expression/BinaryOpExpression.h"

#include <set>

namespace cajeta {
    namespace transform {

        namespace {
            // Recursively lower expression `e` into DAG nodes; returns its node
            // index. Input-param leaves are interned via `paramIdx`. On the first
            // unsupported construct, sets `err` and returns 0 (callers short-circuit).
            size_t buildNode(Expression* e, const std::set<std::string>& params,
                             std::vector<AdNode>& nodes,
                             std::map<std::string, size_t>& paramIdx,
                             std::string& err) {
                if (!err.empty() || !e) return 0;

                if (auto* id = dynamic_cast<IdentifierExpression*>(e)) {
                    std::string name = id->getTextValue();
                    if (params.count(name)) {
                        auto it = paramIdx.find(name);
                        if (it != paramIdx.end()) return it->second;
                        nodes.push_back(AdNode{name, true, "", {}});
                        size_t idx = nodes.size() - 1;
                        paramIdx[name] = idx;
                        return idx;
                    }
                    // A non-parameter identifier is a constant w.r.t. the inputs.
                    nodes.push_back(AdNode{name, false, "", {}});
                    return nodes.size() - 1;
                }

                if (auto* b = dynamic_cast<BinaryOpExpression*>(e)) {
                    std::string prim, opStr;
                    switch (b->getBinaryOp()) {
                        case BINARY_OP_MUL: prim = "mul"; opStr = "*"; break;
                        case BINARY_OP_ADD: prim = "add"; opStr = "+"; break;
                        case BINARY_OP_SUB: prim = "sub"; opStr = "-"; break;
                        default:
                            err = "Grad: unsupported binary operator in the "
                                  "differentiated body (v1: + - *)";
                            return 0;
                    }
                    auto& ch = b->getChildren();
                    if (ch.size() < 2) { err = "Grad: malformed binary expression"; return 0; }
                    size_t li = buildNode(dynamic_cast<Expression*>(ch[0].get()),
                                          params, nodes, paramIdx, err);
                    size_t ri = buildNode(dynamic_cast<Expression*>(ch[1].get()),
                                          params, nodes, paramIdx, err);
                    if (!err.empty()) return 0;
                    std::string val = "(" + nodes[li].valueExpr + " " + opStr + " "
                                    + nodes[ri].valueExpr + ")";
                    nodes.push_back(AdNode{val, false, prim, {li, ri}});
                    return nodes.size() - 1;
                }

                if (auto* p = dynamic_cast<PrefixExpression*>(e)) {
                    if (p->getOp() != PREFIX_OP_NEGATIVE) {
                        err = "Grad: unsupported unary operator in the differentiated "
                              "body (v1: unary -)";
                        return 0;
                    }
                    auto& ch = p->getChildren();
                    if (ch.empty()) { err = "Grad: malformed unary expression"; return 0; }
                    size_t oi = buildNode(dynamic_cast<Expression*>(ch[0].get()),
                                          params, nodes, paramIdx, err);
                    if (!err.empty()) return 0;
                    std::string val = "-(" + nodes[oi].valueExpr + ")";
                    nodes.push_back(AdNode{val, false, "negate", {oi}});
                    return nodes.size() - 1;
                }

                err = "Grad: unsupported expression in the differentiated body "
                      "(v1 supports + - * and unary - over the parameter)";
                return 0;
            }
        } // namespace

        bool buildDag(Expression* body,
                      const std::vector<std::string>& paramNames,
                      std::vector<AdNode>& outNodes,
                      std::map<std::string, size_t>& outParamNodeIndex,
                      std::string* err) {
            std::set<std::string> params(paramNames.begin(), paramNames.end());
            std::string localErr;
            buildNode(body, params, outNodes, outParamNodeIndex, localErr);
            if (!localErr.empty()) { if (err) *err = localErr; return false; }
            return true;
        }

        std::string reverseModeGrad(const std::vector<AdNode>& nodes,
                                    size_t paramIndex,
                                    std::string* missingPrimitive) {
            const VjpRegistry& reg = VjpRegistry::builtin();
            size_t n = nodes.size();
            if (n == 0 || paramIndex >= n) return "";

            // Accumulated cotangent source per node; "" means a zero cotangent.
            std::vector<std::string> cot(n);
            cot[n - 1] = "1.0f";   // seed the output cotangent

            for (size_t i = n; i-- > 0;) {
                const AdNode& nd = nodes[i];
                if (nd.primitive.empty() || cot[i].empty()) continue;
                const VjpRule* rule = reg.lookup(nd.primitive);
                if (!rule) {
                    if (missingPrimitive) *missingPrimitive = nd.primitive;
                    return "";
                }
                std::vector<std::string> operandExprs;
                operandExprs.reserve(nd.operands.size());
                for (size_t oi : nd.operands) operandExprs.push_back(nodes[oi].valueExpr);

                // Parenthesize the incoming cotangent so the rule's fragments compose
                // without precedence surprises.
                std::vector<std::string> contrib =
                    rule->cotangents("(" + cot[i] + ")", operandExprs);

                for (size_t k = 0; k < nd.operands.size() && k < contrib.size(); ++k) {
                    size_t oi = nd.operands[k];
                    cot[oi] = cot[oi].empty() ? contrib[k]
                                              : (cot[oi] + " + " + contrib[k]);
                }
            }
            return cot[paramIndex];
        }

        std::string emitBackwardSource(const std::string& className,
                                       const std::string& paramName,
                                       const std::string& paramTypeName,
                                       const std::string& valueTypeName,
                                       const std::string& gradTypeName,
                                       const std::string& outputValueExpr,
                                       const std::string& gradExpr) {
            std::string gr = "GradResult<" + valueTypeName + "," + gradTypeName + ">";
            std::string fnTy = "(" + paramTypeName + ") -> " + gr;
            return
                "import cajeta.nucleo.transform.GradResult;\n"
                "public class " + className + " {\n"
                "    public static " + fnTy + " make() {\n"
                "        return (" + paramTypeName + " " + paramName + ") -> stack "
                    + gr + "(" + outputValueExpr + ", " + gradExpr + ");\n"
                "    }\n"
                "}\n";
        }

    } // namespace transform
} // namespace cajeta
