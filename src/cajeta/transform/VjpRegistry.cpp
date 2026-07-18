//
// transform-intrinsics Unit 2 — built-in VJP rules (spec §7).
//
// The minimal rule set that validates the mechanism: add / mul / sub / negate /
// matmul / sum.
// Every rule is expressed in built-in primitives, so each rule's own IR is
// differentiable — second-order Grad (§5.1) composes for free on this set.
//
#include "cajeta/transform/VjpRegistry.h"

namespace cajeta {
    namespace transform {

        void VjpRegistry::add(VjpRule rule) {
            rules[rule.primitive] = std::move(rule);
        }

        const VjpRule* VjpRegistry::lookup(const std::string& primitive) const {
            auto it = rules.find(primitive);
            return it == rules.end() ? nullptr : &it->second;
        }

        const VjpRegistry& VjpRegistry::builtin() {
            static const VjpRegistry registry = [] {
                VjpRegistry r;

                // c = a + b  ->  a_bar += g, b_bar += g (surface-independent).
                r.add({"add", 2,
                    [](const std::string& g, const std::vector<std::string>&,
                       const GradSurface&) {
                        return std::vector<std::string>{g, g};
                    }});

                // c = a * b  ->  a_bar += g*b, b_bar += g*a (elementwise on tensors).
                r.add({"mul", 2,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        if (s.tensor) {
                            std::string e = "<" + s.elem + ">";
                            return std::vector<std::string>{
                                "Tensor.mul" + e + "(" + g + ", " + o[1] + ")",
                                "Tensor.mul" + e + "(" + g + ", " + o[0] + ")"};
                        }
                        return std::vector<std::string>{g + " * " + o[1],
                                                        g + " * " + o[0]};
                    }});

                // c = a - b  ->  a_bar += g, b_bar += -g.
                r.add({"sub", 2,
                    [](const std::string& g, const std::vector<std::string>&,
                       const GradSurface& s) {
                        std::string neg = s.tensor
                            ? ("Tensor.mulScalar<" + s.elem + ">(" + g + ", -1.0f)")
                            : ("-(" + g + ")");
                        return std::vector<std::string>{g, neg};
                    }});

                // c = -a  ->  a_bar += -g.
                r.add({"negate", 1,
                    [](const std::string& g, const std::vector<std::string>&,
                       const GradSurface& s) {
                        std::string neg = s.tensor
                            ? ("Tensor.mulScalar<" + s.elem + ">(" + g + ", -1.0f)")
                            : ("-(" + g + ")");
                        return std::vector<std::string>{neg};
                    }});

                // C = A @ B  ->  A_bar += g @ B^T, B_bar += A^T @ g (tensor-only).
                r.add({"matmul", 2,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        std::string e = "<" + s.elem + ">";
                        return std::vector<std::string>{
                            "Tensor.matmul" + e + "(" + g + ", " + o[1] + ".transpose())",
                            "Tensor.matmul" + e + "(" + o[0] + ".transpose(), " + g + ")"};
                    }});

                // s = sum(a)  ->  a_bar += broadcast(g) — the scalar cotangent
                // spread back over a's shape (rank-restoring): ones_like(a) * g.
                r.add({"sum", 1,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        std::string e = "<" + s.elem + ">";
                        return std::vector<std::string>{
                            "Tensor.mulScalar" + e + "(Tensor.onesLike" + e
                                + "(" + o[0] + "), " + g + ")"};
                    }});

                return r;
            }();
            return registry;
        }

    } // namespace transform
} // namespace cajeta
