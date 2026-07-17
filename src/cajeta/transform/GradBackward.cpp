//
// transform-intrinsics Unit 3 — reverse-mode autodiff + Tier-A backward emission.
//
#include "cajeta/transform/GradBackward.h"
#include "cajeta/transform/VjpRegistry.h"

namespace cajeta {
    namespace transform {

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
