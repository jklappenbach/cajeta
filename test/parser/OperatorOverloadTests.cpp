//
// Operator-overload behavioral tests. v1 scope:
//   - Binary arithmetic operators on class types (+, -, *, /, %)
//   - Equality / comparison operators on class types (==, !=, <, >, <=, >=)
//   - The operator method is treated as any other instance method —
//     `this` is implicit, the single named parameter is the RHS, and
//     dispatch goes through the same machinery as `obj.method(arg)`.
//
// Deferred:
//   - Unary operator overloads (++, --, !, ~, unary -)
//   - Compound assignment operators (+=, -=, ...) — methods register, but
//     the BinaryOpExpression dispatch path doesn't yet route to them
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

// Most basic case: `a + b` where a is a class with operator+ defined
// dispatches to the operator method, not the built-in numeric add.
TEST(OperatorOverloadTests, classPlusDispatchesToOperatorMethod) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 operator+ (Counter other) { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter a = new Counter();\n"
        "        Counter b = new Counter();\n"
        "        return a + b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Equality operator: `a == b` for two class instances routes through
// the user-defined operator==.
TEST(OperatorOverloadTests, classEqualsDispatchesToOperatorMethod) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 operator== (Tag other) { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tag a = new Tag();\n"
        "        Tag b = new Tag();\n"
        "        if (a == b) {\n"
        "            return 7;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
