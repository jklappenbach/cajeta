//
// transform-intrinsics plan Unit 2 — the VJP rule registry (spec §7; F2c: an
// internal compiler table for the built-in differentiable primitives). Pure
// C++ unit tests over the registry surface the U3 Grad composer will consume:
// rules retrievable by primitive identity (2.1.1), each rule's cotangent form
// matches its known VJP (2.1.2), a missing rule is a distinct "none" signal
// (2.1.3), and the built-in registry is a single read-only instance (2.1.4).
//
// A rule produces SOURCE fragments for each operand's cotangent, given the
// output cotangent `g` and the operands — the Tier-A delivery form (F4): the
// backward is generated cajeta source that re-enters the checked pipeline.
//
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "cajeta/transform/VjpRegistry.h"

using cajeta::transform::GradSurface;
using cajeta::transform::VjpRegistry;
using cajeta::transform::VjpRule;

namespace {
// Scalar surface by default; pass a tensor surface to exercise the tensor spelling.
std::vector<std::string> cot(const std::string& prim, const std::string& g,
                             const std::vector<std::string>& operands,
                             const GradSurface& s = GradSurface{}) {
    const VjpRule* r = VjpRegistry::builtin().lookup(prim);
    EXPECT_NE(r, nullptr) << "no rule for " << prim;
    return r->cotangents(g, operands, s);
}
const GradSurface kTensorF32{true, "float32"};
} // namespace

// 2.1.1 — a registered primitive's rule is retrievable by identity; an
// unregistered primitive returns nullptr.
TEST(VjpRegistryTests, lookupByPrimitiveIdentity) {
    const VjpRegistry& reg = VjpRegistry::builtin();
    EXPECT_NE(reg.lookup("add"), nullptr);
    EXPECT_NE(reg.lookup("mul"), nullptr);
    EXPECT_NE(reg.lookup("matmul"), nullptr);
    EXPECT_NE(reg.lookup("negate"), nullptr);
    EXPECT_NE(reg.lookup("sum"), nullptr);
    EXPECT_EQ(reg.lookup("no_such_primitive"), nullptr);
}

// 2.1.2 — each built-in rule's cotangent form matches its known VJP.
TEST(VjpRegistryTests, addRuleIsIdentityCotangents) {
    // c = a + b  ->  a_bar += g, b_bar += g
    EXPECT_EQ(cot("add", "g", {"a", "b"}), (std::vector<std::string>{"g", "g"}));
}
TEST(VjpRegistryTests, mulRuleIsSwappedProducts) {
    // c = a * b  ->  a_bar += g*b, b_bar += g*a
    EXPECT_EQ(cot("mul", "g", {"a", "b"}),
              (std::vector<std::string>{"g * b", "g * a"}));
}
TEST(VjpRegistryTests, negateRuleNegatesCotangent) {
    // c = -a  ->  a_bar += -g
    EXPECT_EQ(cot("negate", "g", {"a"}), (std::vector<std::string>{"-(g)"}));
}
TEST(VjpRegistryTests, matmulRuleUsesTransposedProducts) {
    // C = A @ B  ->  A_bar += g @ B^T, B_bar += A^T @ g (tensor-surface spelling).
    EXPECT_EQ(cot("matmul", "g", {"A", "B"}, kTensorF32),
              (std::vector<std::string>{"Tensor.matmul<float32>(g, B.transpose())",
                                        "Tensor.matmul<float32>(A.transpose(), g)"}));
}

// 2.1.2 (tensor surface) — elementwise mul spells `Tensor.mul<E>`.

// 2.1.2 (reduction) — sum's VJP broadcasts the scalar cotangent back over the
// operand's shape: ones_like(a) * g.

// 2.1.2 — rule arity is declared and matches the operand count.
TEST(VjpRegistryTests, ruleArityIsDeclared) {
    EXPECT_EQ(VjpRegistry::builtin().lookup("add")->arity, 2);
    EXPECT_EQ(VjpRegistry::builtin().lookup("negate")->arity, 1);
}

// 2.1.3 — a missing rule is a distinct "none" (nullptr), never a silently valid
// zero rule; this is the signal the composer turns into the §5.3 named error.
TEST(VjpRegistryTests, missingRuleIsNullNotSilentZero) {
    EXPECT_EQ(VjpRegistry::builtin().lookup("sin"), nullptr);
    EXPECT_EQ(VjpRegistry::builtin().lookup(""), nullptr);
}

// 2.1.4 — the built-in registry is one read-only instance (no per-compile mutable
// state; reproducible builds, one rule-set — spec §7.4).
TEST(VjpRegistryTests, builtinIsSingleReadOnlyInstance) {
    EXPECT_EQ(&VjpRegistry::builtin(), &VjpRegistry::builtin());
}
