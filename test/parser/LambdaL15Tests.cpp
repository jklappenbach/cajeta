//
// L1.5 lambda tests — bare-identifier parameters with target-type inference.
// Extends L1's typed-param-only support: when a lambda's params have no
// explicit types, the parameter types come from the surrounding context's
// CajetaFunctionType (set by the LHS of a function-typed variable
// declaration). See docs/specification/lang/Lambdas.md.
//
// Out of scope (L2+):
//   - Captures
//   - Block bodies
//   - `var`-list lambda params
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

// Two bare-name params, types inferred from the LHS function type.
TEST(LambdaL15Tests, bareTwoParamsInfer) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32, int32) -> int32 add = (a, b) -> a + b;\n"
        "        return add(5, 7);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

// Single bare-identifier param `x -> expr`, type inferred.
TEST(LambdaL15Tests, singleBareIdentifierInfers) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 doubler = x -> x * 2;\n"
        "        return doubler(21);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Mixed: same method has typed and bare-param lambdas side by side.
// Verifies the two inference paths don't interfere.
TEST(LambdaL15Tests, mixedTypedAndInferredLambdas) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32, int32) -> int32 explicitAdd = (int32 a, int32 b) -> a + b;\n"
        "        (int32, int32) -> int32 inferredAdd = (a, b) -> a + b;\n"
        "        return explicitAdd(1, 2) + inferredAdd(3, 4);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// Different inferred parameter types in the same function. Confirms each
// lambda picks up its own LHS-pinned types independently.
TEST(LambdaL15Tests, differentInferredTypes) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 i32id = x -> x;\n"
        "        (int64) -> int64 i64id = x -> x;\n"
        "        return i32id(7) + (int32) i64id(11);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18);
}

// Bare-identifier lambda passed as a constructor argument whose
// formal parameter is a function type. The expectedType for the
// lambda's parameter inference must come from the resolved ctor's
// formal — NewExpression has to propagate it the same way
// MethodCallExpression does for method calls. Today (without the
// propagator) this trips CAJETA_ERROR_TYPE_INFERENCE because the
// lambda's bare `acc` / `x` have no signal.
//
// Minimal repro of the ergonomic issue that surfaced during the
// Collectors.toList unblock — once the propagator fires, the stdlib
// body can drop the typed-param spelling too.
TEST(LambdaL15Tests, bareParamsInferFromCtorArgFormal) {
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    public (int32, int32) -> int32 fn;\n"
        "    public Holder(int32 seed, (int32, int32) -> int32 fn) {\n"
        "        this.fn = fn;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder(0, (acc, x) -> acc + x);\n"
        "        return h.fn(10, 5);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}
