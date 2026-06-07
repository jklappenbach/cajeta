// Regression for diamond-operator return typing. `heap Box<>(7)` must resolve
// its STATIC type to the inferred instantiation `Box<int32>` during the
// resolve pass — not the open template `Box` — so a member/method accessed
// directly off the diamond result is typed against the bound parameter (T =
// int32) rather than an uninstantiated T. Before the fix, NewExpression::
// resolveTypes deferred diamond inference to generateCode and left
// resolvedType as the open template for the whole resolve pass, so a T-typed
// member off the result mis-resolved.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// A method returning the template parameter T, invoked directly on the diamond
// result. Only types correctly (as int32) when the diamond resolved to
// Box<int32>; chaining the call forces the result's static type to be right.
TEST(DiamondInferenceTests, methodReturningParamOffDiamondResult) {
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public T value;\n"
        "    public Box(T v) { value = v; }\n"
        "    public T unwrap() { return value; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        return heap Box<>(7).unwrap();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Assigning a T-typed member read off the diamond result into an int32 local:
// well-typed only if T bound to int32 via diamond inference at resolve time.
TEST(DiamondInferenceTests, paramTypedMemberOffDiamondResult) {
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public T value;\n"
        "    public Box(T v) { value = v; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32 r = heap Box<>(11).value;\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}
