//
// L1 lambda tests — the first slice of the lambdas/threading rollout
// (see docs/stdlib/Lambdas.md).
//
// In scope for L1:
//   - Function types `(T1, T2) -> R` as variable types
//   - Non-capturing lambdas with explicit param types
//   - Storing a lambda in a function-typed local
//   - Calling through a function-typed local (indirect call)
//
// Out of scope (later slices):
//   - Captures — body cannot reference outer-scope names
//   - Target-type inference (`(a, b) -> a + b` without explicit types)
//   - Block-body lambdas (only expression bodies in L1)
//   - Method references (L4)
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Smallest case: assign a typed-param lambda to a function-typed local
// and call it. Verifies the parse, the lambda's synthetic-function lowering,
// the function-type-as-pointer storage, and the indirect call path.
TEST(LambdaL1Tests, callTypedAddLambda) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32, int32) -> int32 add = (int32 a, int32 b) -> a + b;\n"
        "        return add(3, 4);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Single-parameter lambda. Confirms the typed-param parsing handles arity 1
// (not just the common 2-arg shape).
TEST(LambdaL1Tests, callSingleArgLambda) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 square = (int32 x) -> x * x;\n"
        "        return square(6);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 36);
}

// Zero-parameter lambda — `() -> 42`. Empty parens, expression body.
TEST(LambdaL1Tests, callZeroArgLambda) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        () -> int32 answer = () -> 42;\n"
        "        return answer();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Two function-typed locals coexisting and being called independently.
// Verifies the synthesis machinery handles multiple lambdas in one method
// without collision (each gets its own __cajeta_lambda_<n> synth name).
TEST(LambdaL1Tests, twoLambdasInOneMethod) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 inc = (int32 x) -> x + 1;\n"
        "        (int32) -> int32 dec = (int32 x) -> x - 1;\n"
        "        return inc(10) + dec(10);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}
