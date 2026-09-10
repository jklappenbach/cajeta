// transform-intrinsics Unit 2 - built-in VJP rules (spec §7), each in built-in primitives.
#include "cajeta/transform/VjpRegistry.h"

namespace cajeta {
    namespace transform {

        void VjpRegistry::add(VjpRule rule) {
            rules[rule.primitive] = std::move(rule);
        }

        const VjpRule* VjpRegistry::lookup(const std::string& primitive) const {
            // Probe hook: CAJETA_NUCLEO_FORCE_BAD_VJP=<primitive> substitutes an ill-typed
            // rule, proving a synthesized backward re-enters the CHECKED pipeline.
            if (const char* bad = std::getenv("CAJETA_NUCLEO_FORCE_BAD_VJP")) {
                if (primitive == bad) {
                    static const VjpRule forcedBad{
                        "__forced_bad", 0,
                        [](const std::string& g,
                           const std::vector<std::string>& operands,
                           const GradSurface&) {
                            return std::vector<std::string>(
                                operands.size(),
                                "__cajeta_forced_bad_vjp(" + g + ")");
                        }};
                    return &forcedBad;
                }
            }
            auto it = rules.find(primitive);
            return it == rules.end() ? nullptr : &it->second;
        }

        const VjpRegistry& VjpRegistry::builtin() {
            static const VjpRegistry registry = [] {
                VjpRegistry r;

                // c = a + b  ->  a_bar += sumTo(g, a), b_bar += sumTo(g, b).
                r.add({"add", 2,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        if (s.tensor) {
                            std::string e = "<" + s.elem + ">";
                            return std::vector<std::string>{
                                "Tensor.sumTo" + e + "(" + g + ", " + o[0] + ")",
                                "Tensor.sumTo" + e + "(" + g + ", " + o[1] + ")"};
                        }
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

                // c = a - b  ->  a_bar += sumTo(g, a), b_bar += sumTo(-g, b)
                r.add({"sub", 2,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        if (s.tensor) {
                            std::string e = "<" + s.elem + ">";
                            return std::vector<std::string>{
                                "Tensor.sumTo" + e + "(" + g + ", " + o[0] + ")",
                                "Tensor.sumTo" + e + "(Tensor.mulScalar" + e
                                    + "(" + g + ", -1.0f), " + o[1] + ")"};
                        }
                        return std::vector<std::string>{g, "-(" + g + ")"};
                    }});

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

                // s = sum(a)  ->  a_bar += broadcast(g), spread back over a's shape.
                r.add({"sum", 1,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        std::string e = "<" + s.elem + ">";
                        return std::vector<std::string>{
                            "Tensor.mulScalar" + e + "(Tensor.onesLike" + e
                                + "(" + o[0] + "), " + g + ")"};
                    }});


                // c = a / b  ->  a_bar += g/b, b_bar += -(g*a)/(b*b).
                r.add({"div", 2,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        if (s.tensor) {
                            std::string e = "<" + s.elem + ">";
                            std::string e3 = "<" + s.elem + ", " + s.elem + ", "
                                + s.elem + ">";
                            return std::vector<std::string>{
                                "Tensor.div" + e3 + "(" + g + ", " + o[1] + ")",
                                "Tensor.mulScalar" + e + "(Tensor.div" + e3
                                    + "(Tensor.mul" + e + "(" + g + ", " + o[0]
                                    + "), Tensor.mul" + e + "(" + o[1] + ", "
                                    + o[1] + ")), -1.0f)"};
                        }
                        return std::vector<std::string>{
                            "(" + g + ") / (" + o[1] + ")",
                            "-((" + g + ") * (" + o[0] + ")) / ((" + o[1]
                                + ") * (" + o[1] + "))"};
                    }});

                // c = exp(a)  ->  a_bar += g * exp(a).
                r.add({"exp", 1,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        if (s.tensor) {
                            std::string e = "<" + s.elem + ">";
                            return std::vector<std::string>{
                                "Tensor.mul" + e + "(" + g + ", Tensor.exp" + e
                                    + "(" + o[0] + "))"};
                        }
                        return std::vector<std::string>{
                            "(" + g + ") * Math.exp(" + o[0] + ")"};
                    }});

                // c = log(a)  ->  a_bar += g / a.
                r.add({"log", 1,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        if (s.tensor) {
                            std::string e3 = "<" + s.elem + ", " + s.elem + ", "
                                + s.elem + ">";
                            return std::vector<std::string>{
                                "Tensor.div" + e3 + "(" + g + ", " + o[0] + ")"};
                        }
                        return std::vector<std::string>{
                            "(" + g + ") / (" + o[0] + ")"};
                    }});

                // c = sqrt(a)  ->  a_bar += g / (2 * sqrt(a)).
                r.add({"sqrt", 1,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        if (s.tensor) {
                            std::string e = "<" + s.elem + ">";
                            std::string e3 = "<" + s.elem + ", " + s.elem + ", "
                                + s.elem + ">";
                            return std::vector<std::string>{
                                "Tensor.div" + e3 + "(" + g + ", Tensor.mulScalar"
                                    + e + "(Tensor.sqrt" + e + "(" + o[0]
                                    + "), 2.0f))"};
                        }
                        return std::vector<std::string>{
                            "(" + g + ") / (2.0f * Math.sqrt(" + o[0] + "))"};
                    }});

                // c = relu(a)  ->  a_bar += g * (a > 0); the mask is not differentiable.
                r.add({"relu", 1,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        if (s.tensor) {
                            std::string e = "<" + s.elem + ">";
                            return std::vector<std::string>{
                                "Tensor.mul" + e + "(" + g + ", Tensor.reluMask"
                                    + e + "(" + o[0] + "))"};
                        }
                        return std::vector<std::string>{
                            "((" + o[0] + ") > 0.0f ? (" + g + ") : 0.0f)"};
                    }});

                // s = mean(a)  ->  sum's rule with numel(a) divided out (tensor-only).
                r.add({"mean", 1,
                    [](const std::string& g, const std::vector<std::string>& o,
                       const GradSurface& s) {
                        std::string e = "<" + s.elem + ">";
                        return std::vector<std::string>{
                            "Tensor.mulScalar" + e + "(Tensor.onesLike" + e
                                + "(" + o[0] + "), (" + g
                                + ") / ((float32) Tensor.productOf(Tensor.shapeOf"
                                + e + "(" + o[0] + "))))"};
                    }});

                return r;
            }();
            return registry;
        }

    } // namespace transform
} // namespace cajeta
