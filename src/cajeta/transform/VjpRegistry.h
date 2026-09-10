// The VJP rule registry: per differentiable primitive, the contribution its
// operands make to the output cotangent. Rules emit SOURCE fragments that re-enter
// the full front end, so a rule composes only checked, differentiable primitives.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cajeta {
    namespace transform {

        // The arithmetic surface a rule emits over: infix operators for scalars,
        // `cajeta.math.Tensor` static calls for tensors, cotangent accumulation too.
        struct GradSurface {
            bool tensor = false;
            std::string elem;        // element type for tensor spellings (e.g. "float32")
            std::string add(const std::string& a, const std::string& b) const {
                return tensor ? ("Tensor.add<" + elem + ">(" + a + ", " + b + ")")
                              : (a + " + " + b);
            }
        };

        // One primitive's rule: `cotangents(g, operands, s)` gives the source for
        // each operand's cotangent contribution, spelled over surface `s`.
        struct VjpRule {
            std::string primitive;   // canonical primitive id
            int arity = 0;           // operand count
            std::function<std::vector<std::string>(
                const std::string& g,
                const std::vector<std::string>& operands,
                const GradSurface& s)> cotangents;
        };

        // The rule table. `builtin()` is one read-only per-process instance, so no
        // per-compile mutable state can make a build unreproducible.
        class VjpRegistry {
        public:
            static const VjpRegistry& builtin();

            // The rule for `primitive`; nullptr becomes a named compile error.
            const VjpRule* lookup(const std::string& primitive) const;

            // Construction / seeding seam (also used by tests).
            VjpRegistry() = default;
            void add(VjpRule rule);

        private:
            std::unordered_map<std::string, VjpRule> rules;
        };

    } // namespace transform
} // namespace cajeta
