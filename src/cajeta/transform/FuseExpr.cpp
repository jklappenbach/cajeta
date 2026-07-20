//
// nucleo-expr Unit 1 — element-level lowering + fused source emission.
//
#include "cajeta/transform/FuseExpr.h"
#include "cajeta/transform/GradBackward.h"

namespace cajeta {
    namespace transform {

        bool fusesElementwise(const std::string& primitive) {
            return primitive == "add" || primitive == "sub"
                || primitive == "mul" || primitive == "div"
                || primitive == "negate" || primitive == "exp"
                || primitive == "log" || primitive == "sqrt";
        }

        std::string elementExpr(const std::vector<AdNode>& nodes, size_t idx,
                                const std::string& indexVar, std::string* err) {
            if (idx >= nodes.size()) {
                if (err) *err = "malformed expression graph";
                return "";
            }
            const AdNode& n = nodes[idx];

            // A leaf: an input tensor reads its element; a scalar constant or
            // non-tensor leaf contributes its source verbatim.
            if (n.primitive.empty()) {
                if (n.isInputParam && n.isTensor) {
                    return n.valueExpr + ".get1(" + indexVar + ")";
                }
                return n.valueExpr;
            }

            if (!fusesElementwise(n.primitive)) {
                if (err) {
                    *err = "'" + n.primitive + "' does not fuse elementwise "
                           "(reductions bound a fusion region)";
                }
                return "";
            }

            std::vector<std::string> ops;
            for (size_t o : n.operands) {
                std::string s = elementExpr(nodes, o, indexVar, err);
                if (s.empty()) return "";
                ops.push_back(s);
            }

            // Scalar arithmetic — the whole point: no Tensor.* call survives
            // into the loop body.
            if (n.primitive == "add" && ops.size() == 2)
                return "(" + ops[0] + " + " + ops[1] + ")";
            if (n.primitive == "sub" && ops.size() == 2)
                return "(" + ops[0] + " - " + ops[1] + ")";
            if (n.primitive == "mul" && ops.size() == 2)
                return "(" + ops[0] + " * " + ops[1] + ")";
            if (n.primitive == "div" && ops.size() == 2)
                return "(" + ops[0] + " / " + ops[1] + ")";
            if (n.primitive == "negate" && ops.size() == 1)
                return "(-(" + ops[0] + "))";
            if (n.primitive == "exp" && ops.size() == 1)
                return "((float32) Math.exp(" + ops[0] + "))";
            if (n.primitive == "log" && ops.size() == 1)
                return "((float32) Math.log(" + ops[0] + "))";
            if (n.primitive == "sqrt" && ops.size() == 1)
                return "((float32) Math.sqrt(" + ops[0] + "))";

            if (err) *err = "'" + n.primitive + "' has an unexpected arity";
            return "";
        }

        std::string emitFusedSource(const std::string& className,
                                    const std::string& paramName,
                                    const std::string& elem,
                                    const std::string& elemExprSrc) {
            std::string e = "<" + elem + ">";
            std::string s;
            s += "import cajeta.math.Tensor;\n";
            s += "import cajeta.lang.Math;\n";
            s += "public final class " + className + " {\n";
            s += "    public static (Tensor" + e + ") -> #Tensor" + e + " make() {\n";
            s += "        return (Tensor" + e + " " + paramName + ") -> {\n";
            s += "            int64 __n = " + paramName + ".size();\n";
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

    } // namespace transform
} // namespace cajeta
