//
// transform-intrinsics Unit 3 — reverse-mode autodiff + Tier-A backward emission.
//
#include "cajeta/transform/GradBackward.h"
#include "cajeta/transform/VjpRegistry.h"

#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/asn/expression/BinaryOpExpression.h"
#include "cajeta/asn/expression/OperatorDispatch.h"
#include "cajeta/asn/expression/MethodCallExpression.h"
#include "cajeta/asn/expression/LiteralExpression.h"
#include "cajeta/type/CajetaType.h"

#include <set>

namespace cajeta {
    namespace transform {

        namespace {
            // The `<A,B,...>` spelling of a call's explicit method type args, or ""
            // if none — reused verbatim to re-inline the forward call source.
            std::string typeArgList(MethodCallExpression* mc) {
                const auto& ta = mc->getExplicitMethodTypeArgs();
                if (ta.empty()) return "";
                std::string s = "<";
                for (size_t i = 0; i < ta.size(); ++i) {
                    if (i) s += ",";
                    s += ta[i] ? ta[i]->toCanonical() : std::string();
                }
                return s + ">";
            }

            // Recursively lower expression `e` into DAG nodes; returns its node
            // index. Input-param leaves are interned via `paramIdx`. On the first
            // unsupported construct, sets `err` and returns 0 (callers short-circuit).
            size_t buildNode(Expression* e,
                             const std::map<std::string, bool>& paramIsTensor,
                             const CallResolver& resolveCall,
                             std::map<std::string, size_t>& bindings,
                             std::vector<AdNode>& nodes,
                             std::map<std::string, size_t>& paramIdx,
                             std::string& err) {
                if (!err.empty() || !e) return 0;

                if (auto* id = dynamic_cast<IdentifierExpression*>(e)) {
                    std::string name = id->getTextValue();
                    // An inlined callee's parameter (U4): resolves to the node the
                    // argument was built into, so gradient flows through the call.
                    auto b = bindings.find(name);
                    if (b != bindings.end()) return b->second;
                    auto rank = paramIsTensor.find(name);
                    if (rank != paramIsTensor.end()) {
                        auto it = paramIdx.find(name);
                        if (it != paramIdx.end()) return it->second;
                        nodes.push_back(AdNode{name, true, "", {}, rank->second});
                        size_t idx = nodes.size() - 1;
                        paramIdx[name] = idx;
                        return idx;
                    }
                    // A non-parameter identifier is a scalar constant w.r.t. inputs.
                    nodes.push_back(AdNode{name, false, "", {}, false});
                    return nodes.size() - 1;
                }

                // A numeric literal is a constant leaf (its source text, zero
                // cotangent contribution) — e.g. `2.0f * x`, `x - 1.0f`.
                if (auto* lit = dynamic_cast<LiteralExpression*>(e)) {
                    nodes.push_back(AdNode{lit->getRawValue(), false, "", {}, false});
                    return nodes.size() - 1;
                }

                if (auto* b = dynamic_cast<BinaryOpExpression*>(e)) {
                    std::string prim, opStr;
                    switch (b->getBinaryOp()) {
                        case BINARY_OP_MUL: prim = "mul"; opStr = "*"; break;
                        case BINARY_OP_ADD: prim = "add"; opStr = "+"; break;
                        case BINARY_OP_SUB: prim = "sub"; opStr = "-"; break;
                        default: {
                            const char* sym = opdispatch::binaryOpSymbol(b->getBinaryOp());
                            err = std::string("unsupported binary operator '")
                                + (sym ? sym : "?")
                                + "' in the transformed body (v1 supports + - * "
                                  "and unary -)";
                            return 0;
                        }
                    }
                    auto& ch = b->getChildren();
                    if (ch.size() < 2) { err = "Grad: malformed binary expression"; return 0; }
                    size_t li = buildNode(dynamic_cast<Expression*>(ch[0].get()),
                                          paramIsTensor, resolveCall, bindings, nodes, paramIdx, err);
                    size_t ri = buildNode(dynamic_cast<Expression*>(ch[1].get()),
                                          paramIsTensor, resolveCall, bindings, nodes, paramIdx, err);
                    if (!err.empty()) return 0;
                    std::string val = "(" + nodes[li].valueExpr + " " + opStr + " "
                                    + nodes[ri].valueExpr + ")";
                    // Scalar arithmetic operators only appear in scalar bodies.
                    nodes.push_back(AdNode{val, false, prim, {li, ri}, false});
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
                                          paramIsTensor, resolveCall, bindings, nodes, paramIdx, err);
                    if (!err.empty()) return 0;
                    std::string val = "-(" + nodes[oi].valueExpr + ")";
                    nodes.push_back(AdNode{val, false, "negate", {oi}, false});
                    return nodes.size() - 1;
                }

                // Static tensor op — `Tensor.<op>(args...)`. The binary elementwise
                // ops (mul/add/sub/matmul) take 2 operands; `sum` reduces 1 tensor
                // to a scalar. The forward call source is re-inlined from the (pure)
                // operand sources and the element type the user wrote at the call.
                if (auto* mc = dynamic_cast<MethodCallExpression*>(e)) {
                    const auto& mcCh = mc->getChildren();
                    std::string recv;
                    if (!mcCh.empty()) {
                        if (auto* rid = dynamic_cast<IdentifierExpression*>(mcCh[0].get()))
                            recv = rid->getTextValue();
                    }
                    const std::string& op = mc->getMethodCallName();
                    static const std::set<std::string> tensorOps =
                        {"mul", "add", "sub", "matmul", "sum"};
                    if (recv == "Tensor" && tensorOps.count(op)) {
                        const auto& args = mc->getParameters();
                        size_t nOperands = (op == "sum") ? 1 : 2;
                        if (args.size() < nOperands) {
                            err = "Grad: malformed Tensor." + op
                                + " in the differentiated body";
                            return 0;
                        }
                        std::vector<size_t> operandIdx;
                        for (size_t k = 0; k < nOperands; ++k) {
                            auto* ae = dynamic_cast<Expression*>(args[k].expression.get());
                            size_t ci = buildNode(ae, paramIsTensor, resolveCall, bindings, nodes,
                                                  paramIdx, err);
                            if (!err.empty()) return 0;
                            operandIdx.push_back(ci);
                        }
                        std::string ta = typeArgList(mc);
                        // 8.1.2 — rank validation with the statically-known
                        // rank-kind and dtype (dims are not in the type system).
                        // matmul CONTRACTS two tensors and sum REDUCES one, so a
                        // scalar operand there is definitively wrong; elementwise
                        // ops are left alone (a scalar operand may be a broadcast).
                        auto rankOf = [&](size_t idx) {
                            return nodes[idx].isTensor ? "tensor" : "scalar";
                        };
                        if (op == "matmul" && (!nodes[operandIdx[0]].isTensor
                                               || !nodes[operandIdx[1]].isTensor)) {
                            err = "Tensor.matmul" + ta + " rank mismatch: left operand"
                                  " is " + rankOf(operandIdx[0]) + ", right operand is "
                                + rankOf(operandIdx[1])
                                + " (matmul contracts two tensors)";
                            return 0;
                        }
                        if (op == "sum" && !nodes[operandIdx[0]].isTensor) {
                            err = "Tensor.sum" + ta
                                + " expects a tensor operand, got scalar";
                            return 0;
                        }
                        std::string val = "Tensor." + op + ta + "(";
                        for (size_t k = 0; k < nOperands; ++k) {
                            if (k) val += ", ";
                            val += nodes[operandIdx[k]].valueExpr;
                        }
                        val += ")";
                        // Elementwise ops stay tensor-ranked; `sum` reduces to scalar.
                        nodes.push_back(AdNode{val, false, op, operandIdx, op != "sum"});
                        return nodes.size() - 1;
                    }

                    // U4 — a call to a user helper. The resolver (compiler core)
                    // maps it to a differentiate-through inline target or a @NoGrad
                    // stop-gradient. Build each argument node FIRST (in the caller's
                    // binding context) so they carry the right forward source and,
                    // for the inline case, the gradient inputs.
                    const auto& args = mc->getParameters();
                    InlineTarget t = resolveCall ? resolveCall(op, args.size())
                                                 : InlineTarget{};
                    if (t.found) {
                        std::vector<size_t> argIdx;
                        argIdx.reserve(args.size());
                        for (const auto& a : args) {
                            auto* ae = dynamic_cast<Expression*>(a.expression.get());
                            size_t ci = buildNode(ae, paramIsTensor, resolveCall,
                                                  bindings, nodes, paramIdx, err);
                            if (!err.empty()) return 0;
                            argIdx.push_back(ci);
                        }
                        if (t.noGrad) {
                            // Stop-gradient: forward value = qualified call over the
                            // arg sources; a leaf (no operands) -> zero cotangent, no
                            // backward term. The value is preserved so f's value is
                            // correct (spec §9.2).
                            std::string val = t.qualifiedName + "(";
                            for (size_t k = 0; k < argIdx.size(); ++k) {
                                if (k) val += ", ";
                                val += nodes[argIdx[k]].valueExpr;
                            }
                            val += ")";
                            nodes.push_back(AdNode{val, false, "", {}, t.returnIsTensor});
                            return nodes.size() - 1;
                        }
                        // Differentiate through: bind the callee's params to the arg
                        // nodes (shadowing any same-named binding) and walk its
                        // single return expression, so cotangents flow into the args.
                        if (t.paramNames.size() != args.size() || !t.body) {
                            err = "Grad: cannot inline '" + op + "' — it must be a "
                                  "single-return-expression function whose parameter "
                                  "count matches the call";
                            return 0;
                        }
                        std::vector<std::pair<std::string, size_t>> shadowed;
                        std::vector<std::string> added;
                        for (size_t k = 0; k < t.paramNames.size(); ++k) {
                            auto it = bindings.find(t.paramNames[k]);
                            if (it != bindings.end()) shadowed.push_back(*it);
                            else added.push_back(t.paramNames[k]);
                            bindings[t.paramNames[k]] = argIdx[k];
                        }
                        size_t r = buildNode(t.body, paramIsTensor, resolveCall,
                                             bindings, nodes, paramIdx, err);
                        for (const auto& s : shadowed) bindings[s.first] = s.second;
                        for (const auto& n : added) bindings.erase(n);
                        if (!err.empty()) return 0;
                        return r;
                    }
                }

                err = "Grad: unsupported expression in the differentiated body "
                      "(v1 supports + - * and unary - over scalars, and "
                      "Tensor.mul/add/sub/matmul/sum over tensors)";
                return 0;
            }
        } // namespace

        bool buildDag(Expression* body,
                      const std::vector<std::string>& paramNames,
                      const std::map<std::string, bool>& paramIsTensor,
                      const CallResolver& resolveCall,
                      std::vector<AdNode>& outNodes,
                      std::map<std::string, size_t>& outParamNodeIndex,
                      std::string* err) {
            (void)paramNames;   // membership + rank both come from paramIsTensor
            std::string localErr;
            std::map<std::string, size_t> bindings;   // inlined-callee params (U4)
            buildNode(body, paramIsTensor, resolveCall, bindings, outNodes,
                      outParamNodeIndex, localErr);
            if (!localErr.empty()) { if (err) *err = localErr; return false; }
            return true;
        }

        std::string reverseModeGrad(const std::vector<AdNode>& nodes,
                                    size_t paramIndex,
                                    const std::string& elem,
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

                // The rule emits over the tensor surface if it touches a tensor —
                // `sum`'s node is a scalar but its operand (and its rule) are tensor.
                bool ruleTensor = nd.isTensor;
                for (size_t oi : nd.operands) ruleTensor = ruleTensor || nodes[oi].isTensor;
                GradSurface ruleSurf{ruleTensor, elem};

                // Parenthesize the incoming cotangent so the rule's fragments compose
                // without precedence surprises.
                std::vector<std::string> contrib =
                    rule->cotangents("(" + cot[i] + ")", operandExprs, ruleSurf);

                for (size_t k = 0; k < nd.operands.size() && k < contrib.size(); ++k) {
                    size_t oi = nd.operands[k];
                    // Accumulate over the OPERAND's own surface (its rank decides `+`
                    // vs `Tensor.add<E>`).
                    GradSurface accSurf{nodes[oi].isTensor, elem};
                    cot[oi] = cot[oi].empty() ? contrib[k]
                                              : accSurf.add(cot[oi], contrib[k]);
                }
            }
            return cot[paramIndex];
        }

        std::string emitBackwardSource(const std::string& className,
                                       const std::vector<std::string>& paramNames,
                                       const std::vector<std::string>& paramTypeNames,
                                       const std::string& valueTypeName,
                                       const std::string& gradTypeName,
                                       const std::string& outputValueExpr,
                                       const std::string& gradExpr,
                                       bool importTensor) {
            std::string gr = "GradResult<" + valueTypeName + "," + gradTypeName + ">";
            // `(T0,T1) -> GR` for make()'s return type; `(T0 p0, T1 p1)` for the
            // returned lambda's param list — the closure keeps f's full arity.
            std::string sig, plist;
            for (size_t i = 0; i < paramNames.size(); ++i) {
                if (i) { sig += ","; plist += ", "; }
                sig += paramTypeNames[i];
                plist += paramTypeNames[i] + " " + paramNames[i];
            }
            std::string fnTy = "(" + sig + ") -> " + gr;
            return
                (importTensor ? std::string("import cajeta.math.Tensor;\n")
                              : std::string())
              + "import cajeta.nucleo.transform.GradResult;\n"
                "public class " + className + " {\n"
                "    public static " + fnTy + " make() {\n"
                "        return (" + plist + ") -> stack "
                    + gr + "(" + outputValueExpr + ", " + gradExpr + ");\n"
                "    }\n"
                "}\n";
        }

    } // namespace transform
} // namespace cajeta
