//
// transform-intrinsics Unit 2 — the VJP rule registry (spec §7).
//
// Each differentiable primitive declares its vector-Jacobian-product rule: given
// the output cotangent and the primitive's operands, the contribution to each
// operand's cotangent. `Grad` (Unit 3) walks a target's mid-level IR, looks each
// primitive's rule up here, and composes them in reverse to build the backward.
//
// F2c: the built-in primitives are core-owned, so their rules live in a
// compiler-internal table (the `@Vjp` annotation surface for third-party rules
// is deferred). The rules produce SOURCE fragments (F4, Tier-A delivery): the
// backward is generated cajeta source that re-enters parse -> type-check ->
// borrow-check -> codegen, so a rule can only compose existing checked
// primitives, and its own IR is itself differentiable (enables §5.1 second-order).
//
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cajeta {
    namespace transform {

        // The arithmetic surface a rule emits over. Scalar primitives use infix
        // operators (`a + b`); tensor primitives use `cajeta.math.Tensor` static
        // calls, which need the element-type spelling. The one abstraction that
        // varies between a scalar `x*x` body and a tensor `sum(x*x)` body: it also
        // spells cotangent accumulation (`+` vs `Tensor.add<E>`) for the composer.
        struct GradSurface {
            bool tensor = false;
            std::string elem;        // element type for tensor spellings (e.g. "float32")
            std::string add(const std::string& a, const std::string& b) const {
                // Scalar accumulation stays bare (`a + b`) — it is re-parenthesized
                // at every use site; the tensor form is a call, so needs none.
                return tensor ? ("Tensor.add<" + elem + ">(" + a + ", " + b + ")")
                              : (a + " + " + b);
            }
        };

        // A VJP rule for one differentiable primitive. `cotangents(g, operands, s)`
        // returns the source expression for each operand's cotangent contribution,
        // given the output-cotangent source `g`, the operand sources, and the
        // arithmetic surface `s` to spell them over. e.g. mul(a,b) on the scalar
        // surface: { "g * b", "g * a" }; on the tensor surface:
        // { "Tensor.mul<E>(g, b)", "Tensor.mul<E>(g, a)" }.
        struct VjpRule {
            std::string primitive;   // canonical primitive id
            int arity = 0;           // operand count
            std::function<std::vector<std::string>(
                const std::string& g,
                const std::vector<std::string>& operands,
                const GradSurface& s)> cotangents;
        };

        // The compiler-internal VJP rule table. `builtin()` is the single,
        // read-only, per-process instance seeded with the built-in rules — no
        // per-compile mutable state (reproducible builds; one rule-set, two
        // drivers — the eager tape in nucleo-autograd-spec.md consumes the same).
        class VjpRegistry {
        public:
            static const VjpRegistry& builtin();

            // The rule for `primitive`, or nullptr if none — the "missing rule"
            // signal the composer turns into the §5.3 named compile error.
            const VjpRule* lookup(const std::string& primitive) const;

            // Construction / seeding seam (also used by tests).
            VjpRegistry() = default;
            void add(VjpRule rule);

        private:
            std::unordered_map<std::string, VjpRule> rules;
        };

    } // namespace transform
} // namespace cajeta
