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

TEST(EnhancedForTests, sumIntArray) {
    EXPECT_EQ(runI32(
        "int32[] xs = new int32[5];\n"
        "xs[0] = 1;\n"
        "xs[1] = 2;\n"
        "xs[2] = 3;\n"
        "xs[3] = 4;\n"
        "xs[4] = 5;\n"
        "int32 total = 0;\n"
        "for (int32 x : xs) {\n"
        "    total = total + x;\n"
        "}\n"
        "return total;"), 15);
}

TEST(EnhancedForTests, emptyArrayLeavesAccumUnchanged) {
    EXPECT_EQ(runI32(
        "int32[] xs = new int32[0];\n"
        "int32 total = 99;\n"
        "for (int32 x : xs) {\n"
        "    total = 0;\n"
        "}\n"
        "return total;"), 99);
}

TEST(EnhancedForTests, iteratorBindingExposesIndex) {
    // Cajeta extension: `int i, T x` form pairs the index with the element binding.
    EXPECT_EQ(runI32(
        "int32[] xs = new int32[4];\n"
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

TEST(EnhancedForTests, breakStopsIteration) {
    EXPECT_EQ(runI32(
        "int32[] xs = new int32[10];\n"
        "for (int32 i = 0; i < 10; i = i + 1) { xs[i] = i; }\n"
        "int32 sum = 0;\n"
        "for (int32 x : xs) {\n"
        "    if (x == 5) break;\n"
        "    sum = sum + x;\n"
        "}\n"
        // 0+1+2+3+4 = 10 (break fires before 5 is added)
        "return sum;"), 10);
}

TEST(EnhancedForTests, continueSkipsElement) {
    EXPECT_EQ(runI32(
        "int32[] xs = new int32[5];\n"
        "xs[0] = 1;\n"
        "xs[1] = 2;\n"
        "xs[2] = 3;\n"
        "xs[3] = 4;\n"
        "xs[4] = 5;\n"
        "int32 sum = 0;\n"
        "for (int32 x : xs) {\n"
        "    if (x % 2 == 0) continue;\n"
        "    sum = sum + x;\n"
        "}\n"
        // 1+3+5 = 9
        "return sum;"), 9);
}

TEST(EnhancedForTests, iterateStringArray) {
    EXPECT_EQ(runI32(
        "String[] names = new String[3];\n"
        "names[0] = \"a\";\n"
        "names[1] = \"bb\";\n"
        "names[2] = \"ccc\";\n"
        "int32 totalLen = 0;\n"
        "for (String n : names) {\n"
        "    totalLen = totalLen + (int32) n.size();\n"
        "}\n"
        "return totalLen;"), 6);
}
