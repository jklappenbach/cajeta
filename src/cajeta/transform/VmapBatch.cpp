//
// transform-intrinsics Unit 5 — Vmap batching rules + Tier-A batched source.
//
#include "cajeta/transform/VmapBatch.h"

namespace cajeta {
    namespace transform {

        bool hasBatchRule(const std::string& primitive) {
            return primitive == "add" || primitive == "sub"
                || primitive == "mul" || primitive == "negate";
        }

        std::string emitBatchedSource(const std::string& className,
                                      const std::string& paramName,
                                      const std::string& paramTypeName,
                                      const std::string& resultTypeName,
                                      const std::string& bodyExpr,
                                      bool importGradResult) {
            std::string s;
            if (importGradResult) {
                s += "import cajeta.nucleo.transform.GradResult;\n";
            }
            s += "public final class " + className + " {\n";
            s += "    public static (" + paramTypeName + "[]) -> #"
               + resultTypeName + "[] make() {\n";
            s += "        return (" + paramTypeName + "[] __xs) -> {\n";
            s += "            " + resultTypeName + "[] __out = heap "
               + resultTypeName + "[__xs.count()];\n";
            s += "            for (int32 __i = 0; __i < __xs.count(); __i = __i + 1) {\n";
            s += "                " + paramTypeName + " " + paramName
               + " = __xs[__i];\n";
            s += "                __out[__i] = " + bodyExpr + ";\n";
            s += "            }\n";
            s += "            return #__out;\n";
            s += "        };\n";
            s += "    }\n";
            s += "}\n";
            return s;
        }

    } // namespace transform
} // namespace cajeta
