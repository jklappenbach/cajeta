//
// Tests for System.exit / System.currentTimeMillis and Math.random.
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
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

double runF64(const std::string& body) {
    std::string src =
        "package test;\n"
        "public final class S {\n"
        "    public static float64 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<double (*)()>("run");
    return fn();
}

} // namespace

TEST(SystemUtilTests, currentTimeMillisIsPositive) {
    // Wall-clock millis since epoch is always > 0 on any sane host.
    EXPECT_EQ(runI32(
        "int64 t = System.currentTimeMillis();\n"
        "if (t > 0) return 1;\n"
        "return 0;"), 1);
}

TEST(SystemUtilTests, currentTimeMillisMonotonicAcrossTwoReads) {
    // Two back-to-back reads — clock either advances or stays the same.
    EXPECT_EQ(runI32(
        "int64 a = System.currentTimeMillis();\n"
        "int64 b = System.currentTimeMillis();\n"
        "if (b >= a) return 1;\n"
        "return 0;"), 1);
}

TEST(SystemUtilTests, randomInUnitInterval) {
    // Math.random() returns [0.0, 1.0).
    double r = runF64("return Math.random();");
    EXPECT_GE(r, 0.0);
    EXPECT_LT(r, 1.0);
}

TEST(SystemUtilTests, randomDistinctOnConsecutiveCalls) {
    // Two calls in the same JIT run get different draws — degenerate-PRNG check.
    EXPECT_EQ(runI32(
        "float64 a = Math.random();\n"
        "float64 b = Math.random();\n"
        "if (a != b) return 1;\n"
        "return 0;"), 1);
}
