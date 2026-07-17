//
// transform-intrinsics Unit 2 — built-in VJP rules (spec §7).
//
// The minimal rule set that validates the mechanism: add / mul / negate / matmul.
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

                // c = a + b  ->  a_bar += g, b_bar += g
                r.add({"add", 2,
                    [](const std::string& g, const std::vector<std::string>&) {
                        return std::vector<std::string>{g, g};
                    }});

                // c = a * b  ->  a_bar += g*b, b_bar += g*a
                r.add({"mul", 2,
                    [](const std::string& g, const std::vector<std::string>& o) {
                        return std::vector<std::string>{g + " * " + o[1],
                                                        g + " * " + o[0]};
                    }});

                // c = -a  ->  a_bar += -g
                r.add({"negate", 1,
                    [](const std::string& g, const std::vector<std::string>&) {
                        return std::vector<std::string>{"-(" + g + ")"};
                    }});

                // C = A @ B  ->  A_bar += g @ B^T, B_bar += A^T @ g
                r.add({"matmul", 2,
                    [](const std::string& g, const std::vector<std::string>& o) {
                        return std::vector<std::string>{
                            "matmul(" + g + ", transpose(" + o[1] + "))",
                            "matmul(transpose(" + o[0] + "), " + g + ")"};
                    }});

                return r;
            }();
            return registry;
        }

    } // namespace transform
} // namespace cajeta
