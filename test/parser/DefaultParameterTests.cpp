//
// Default parameter values: `int32 foo(int32 x = 42) { ... }` — when the
// caller omits an argument, the parameter's default expression is
// evaluated at the call site and substituted in.
//
// v1 scope: trailing defaults only (the standard form); single missing
// default at a time; literal-expression defaults. Closures / call-site-
// scoped name captures aren't exercised yet.
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

// Single trailing default. Caller omits the arg; method returns the default.
TEST(DefaultParameterTests, singleTrailingDefault) {
    auto src =
        "package test;\n"
        "public class Calc {\n"
        "    public int32 echo(int32 x = 42) { return x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Calc c = heap Calc();\n"
        "        return c.echo();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Caller supplies the arg explicitly — overrides the default.
TEST(DefaultParameterTests, explicitArgOverridesDefault) {
    auto src =
        "package test;\n"
        "public class Calc {\n"
        "    public int32 echo(int32 x = 42) { return x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Calc c = heap Calc();\n"
        "        return c.echo(99);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Multiple trailing defaults, partial caller supply.
TEST(DefaultParameterTests, partialFillFromDefaults) {
    auto src =
        "package test;\n"
        "public class Calc {\n"
        "    public int32 add(int32 a, int32 b = 10, int32 c = 100) {\n"
        "        return a + b + c;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Calc calc = heap Calc();\n"
        "        return calc.add(1);\n"
        "    }\n"
        "}\n";
    // a=1, b=10 (default), c=100 (default)  => 111
    EXPECT_EQ(runI32(src), 111);
}
