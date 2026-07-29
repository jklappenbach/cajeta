//
// nucleo-expr — element-level lowering, reduction staging, fused emission.
//
#include "cajeta/transform/FuseExpr.h"
#include "cajeta/transform/GradBackward.h"

namespace cajeta {
    namespace transform {

        bool fusesElementwise(const std::string& primitive) {
            return primitive == "add" || primitive == "sub"
                || primitive == "mul" || primitive == "div"
                || primitive == "negate" || primitive == "exp"
                || primitive == "log" || primitive == "sqrt"
                // Scalar-broadcast family: tensor op loop-invariant scalar.
                || primitive == "addScalar" || primitive == "subScalar"
                || primitive == "mulScalar" || primitive == "divScalar";
        }

        bool isReduction(const std::string& primitive) {
            return primitive == "sum" || primitive == "mean"
                || primitive == "std";
        }

        std::string elementExpr(const std::vector<AdNode>& nodes, size_t idx,
                                const std::string& indexVar,
                                std::vector<Hoist>* hoists, std::string* err) {
            if (idx >= nodes.size()) {
                if (err) *err = "malformed expression graph";
                return "";
            }
            const AdNode& n = nodes[idx];

            // A leaf: an input tensor reads its element; a scalar constant or
            // non-tensor leaf contributes its source verbatim. THE COLUMN SEAM
            // (plan X7-seam) is this get1 — the only dense-buffer assumption.
            if (n.primitive.empty()) {
                if (n.isInputParam && n.isTensor) {
                    return n.valueExpr + ".get1(" + indexVar + ")";
                }
                return n.valueExpr;
            }

            // A reduction is loop-INVARIANT: stage it in the preheader and
            // reference the local inside the loop (spec 3.3).
            if (isReduction(n.primitive)) {
                if (!hoists) {
                    if (err) {
                        *err = "'" + n.primitive + "' reduces and cannot appear here";
                    }
                    return "";
                }
                for (const auto& h : *hoists) {          // CSE: stage once
                    if (h.init == n.valueExpr) return h.local;
                }
                std::string local = "__r" + std::to_string(hoists->size());
                hoists->push_back(Hoist{local, n.valueExpr});
                return local;
            }

            if (!fusesElementwise(n.primitive)) {
                if (err) {
                    *err = "'" + n.primitive + "' does not fuse elementwise";
                }
                return "";
            }

            std::vector<std::string> ops;
            for (size_t o : n.operands) {
                std::string s = elementExpr(nodes, o, indexVar, hoists, err);
                if (s.empty()) return "";
                ops.push_back(s);
            }

            // Scalar arithmetic — the whole point: no Tensor.* call survives
            // into the loop body. The scalar-broadcast ops lower to exactly the
            // same arithmetic as their elementwise twins; the difference was
            // only ever whether the right operand varies with the index.
            if (ops.size() == 2) {
                if (n.primitive == "add" || n.primitive == "addScalar")
                    return "(" + ops[0] + " + " + ops[1] + ")";
                if (n.primitive == "sub" || n.primitive == "subScalar")
                    return "(" + ops[0] + " - " + ops[1] + ")";
                if (n.primitive == "mul" || n.primitive == "mulScalar")
                    return "(" + ops[0] + " * " + ops[1] + ")";
                if (n.primitive == "div" || n.primitive == "divScalar")
                    return "(" + ops[0] + " / " + ops[1] + ")";
            }
            if (ops.size() == 1) {
                if (n.primitive == "negate") return "(-(" + ops[0] + "))";
                if (n.primitive == "exp")
                    return "((float32) Math.exp(" + ops[0] + "))";
                if (n.primitive == "log")
                    return "((float32) Math.log(" + ops[0] + "))";
                if (n.primitive == "sqrt")
                    return "((float32) Math.sqrt(" + ops[0] + "))";
            }

            if (err) *err = "'" + n.primitive + "' has an unexpected arity";
            return "";
        }

        namespace {
            std::string hoistBlock(const std::vector<Hoist>& hoists,
                                   const std::string& elem) {
                std::string s;
                for (const auto& h : hoists) {
                    s += "            " + elem + " " + h.local + " = "
                       + h.init + ";\n";
                }
                return s;
            }
        } // namespace

        std::string emitFusedSource(const std::string& className,
                                    const std::string& paramName,
                                    const std::string& elem,
                                    const std::string& elemExprSrc,
                                    const std::vector<Hoist>& hoists) {
            std::string e = "<" + elem + ">";
            std::string s;
            s += "import cajeta.math.Tensor;\n";
            s += "import cajeta.lang.Math;\n";
            s += "public final class " + className + " {\n";
            s += "    public static (Tensor" + e + ") -> #Tensor" + e + " make() {\n";
            s += "        return (Tensor" + e + " " + paramName + ") -> {\n";
            s += "            int64 __n = " + paramName + ".size();\n";
            s += hoistBlock(hoists, elem);
            s += "            Tensor" + e + " __out = Tensor.zerosLike" + e
               + "(" + paramName + ");\n";
            s += "            for (int64 __i = 0; __i < __n; __i = __i + 1) {\n";
            s += "                " + elem + " __v = " + elemExprSrc + ";\n";
            s += "                __out.set1(__i, __v);\n";
            s += "            }\n";
            s += "            return #__out;\n";
            s += "        };\n";
            s += "    }\n";
            s += "}\n";
            return s;
        }

        std::string emitFusedReductionSource(const std::string& className,
                                             const std::string& paramName,
                                             const std::string& elem,
                                             const std::string& reductionPrim,
                                             const std::string& elemExprSrc,
                                             const std::vector<Hoist>& hoists) {
            std::string e = "<" + elem + ">";
            std::string s;
            s += "import cajeta.math.Tensor;\n";
            s += "import cajeta.lang.Math;\n";
            s += "public final class " + className + " {\n";
            s += "    public static (Tensor" + e + ") -> " + elem + " make() {\n";
            s += "        return (Tensor" + e + " " + paramName + ") -> {\n";
            s += "            int64 __n = " + paramName + ".size();\n";
            s += hoistBlock(hoists, elem);
            // The elementwise body fuses INTO the accumulation: no temporary
            // tensor is built for the reduction's input.
            s += "            " + elem + " __acc = 0.0f;\n";
            s += "            for (int64 __i = 0; __i < __n; __i = __i + 1) {\n";
            s += "                __acc = __acc + " + elemExprSrc + ";\n";
            s += "            }\n";
            if (reductionPrim == "mean") {
                s += "            return __acc / ((" + elem + ") __n);\n";
            } else {
                s += "            return __acc;\n";
            }
            s += "        };\n";
            s += "    }\n";
            s += "}\n";
            return s;
        }

    } // namespace transform
} // namespace cajeta
