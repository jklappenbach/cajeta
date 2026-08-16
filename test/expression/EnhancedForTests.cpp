//
// Tests for the enhanced-for form `for (T x : arr)` and the Cajeta-extended
// `for (int i, T x : arr)` form that exposes the running index.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "public final class F {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.F");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace



TEST(EnhancedForTests, iteratorBindingExposesIndex) {
    // Cajeta extension: `int i, T x` form pairs the index with the element binding.
    EXPECT_EQ(runI32(
        "int32[] xs = heap int32[4];\n"
        "xs[0] = 10;\n"
        "xs[1] = 20;\n"
        "xs[2] = 30;\n"
        "xs[3] = 40;\n"
        "int32 acc = 0;\n"
        "for (int32 i, int32 x : xs) {\n"
        "    acc = acc + i * x;\n"
        "}\n"
        // 0*10 + 1*20 + 2*30 + 3*40 = 200
        "return acc;"), 200);
}



