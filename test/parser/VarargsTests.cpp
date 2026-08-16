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
        "        Calc c = heap Calc();\n"
        "        return c.second(10, 20, 30);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

// count() on the packed array reports the trailing-arg count.

// Mixing fixed and varargs: a leading fixed param followed by trailing
// `int32...`. Fixed param threads through normally; the rest pack.
