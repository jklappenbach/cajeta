//
// Varargs (`T... args`) behavioral tests. v1 scope:
//   - Trailing `T... args` parameter on instance methods
//   - Callers passing 0..N trailing args; they're packed into a `T[]`
//   - The callee sees `args` as a regular T[] (size + indexable)
//
// Deferred:
//   - Static varargs methods (no test today; same machinery should work)
//   - Passing an existing T[] directly to a varargs slot (Java's pass-through)
//   - Varargs of class types
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

// Smallest case: three trailing args become a length-3 array, accessed by
// index inside the method.
TEST(VarargsTests, threeArgsPackIntoArray) {
    auto src =
        "package test;\n"
        "public class Calc {\n"
        "    public int32 second(int32... xs) { return xs[1]; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Calc c = new Calc();\n"
        "        return c.second(10, 20, 30);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

// size() on the packed array reports the trailing-arg count.
TEST(VarargsTests, sizeMatchesTrailingArgCount) {
    auto src =
        "package test;\n"
        "public class Calc {\n"
        "    public int32 count(int32... xs) { return (int32) xs.size(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Calc c = new Calc();\n"
        "        return c.count(1, 2, 3, 4, 5);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Mixing fixed and varargs: a leading fixed param followed by trailing
// `int32...`. Fixed param threads through normally; the rest pack.
TEST(VarargsTests, fixedPlusVarargs) {
    auto src =
        "package test;\n"
        "public class Calc {\n"
        "    public int32 sumWithBase(int32 base, int32... xs) {\n"
        "        return base + xs[0] + xs[1];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Calc c = new Calc();\n"
        "        return c.sumWithBase(100, 5, 7);\n"
        "    }\n"
        "}\n";
    // 100 + 5 + 7 = 112
    EXPECT_EQ(runI32(src), 112);
}
