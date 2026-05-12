//
// Tests for the Math.* intrinsics, lowered to LLVM intrinsics inline.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSourceI32(const std::string& body) {
    return "package test;\n"
           "public final class M {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

std::string makeSourceF64(const std::string& body) {
    return "package test;\n"
           "public final class M {\n"
           "    public static float64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runI32(const std::string& body) {
    auto jit = CajetaJit::compile(makeSourceI32(body), "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

double runF64(const std::string& body) {
    auto jit = CajetaJit::compile(makeSourceF64(body), "test.M");
    auto fn = jit->lookup<double (*)()>("run");
    return fn();
}

} // namespace

TEST(MathIntrinsicTests, absIntNegative) {
    EXPECT_EQ(runI32("return (int32) Math.abs(-42);"), 42);
}

TEST(MathIntrinsicTests, absIntPositive) {
    EXPECT_EQ(runI32("return (int32) Math.abs(7);"), 7);
}

TEST(MathIntrinsicTests, absFloat) {
    EXPECT_DOUBLE_EQ(runF64("return Math.abs(-3.5);"), 3.5);
}

TEST(MathIntrinsicTests, maxIntChoosesLarger) {
    EXPECT_EQ(runI32("return (int32) Math.max(3, 7);"), 7);
}

TEST(MathIntrinsicTests, minIntChoosesSmaller) {
    EXPECT_EQ(runI32("return (int32) Math.min(3, 7);"), 3);
}

TEST(MathIntrinsicTests, maxFloatChoosesLarger) {
    EXPECT_DOUBLE_EQ(runF64("return Math.max(1.5, 2.25);"), 2.25);
}

TEST(MathIntrinsicTests, sqrtBasic) {
    EXPECT_DOUBLE_EQ(runF64("return Math.sqrt(16.0);"), 4.0);
}

TEST(MathIntrinsicTests, powBasic) {
    EXPECT_DOUBLE_EQ(runF64("return Math.pow(2.0, 10.0);"), 1024.0);
}

TEST(MathIntrinsicTests, floorRoundsDown) {
    EXPECT_DOUBLE_EQ(runF64("return Math.floor(3.7);"), 3.0);
}

TEST(MathIntrinsicTests, ceilRoundsUp) {
    EXPECT_DOUBLE_EQ(runF64("return Math.ceil(3.2);"), 4.0);
}

TEST(MathIntrinsicTests, roundHalfUp) {
    EXPECT_EQ(runI32("return (int32) Math.round(3.5);"), 4);
}

TEST(MathIntrinsicTests, roundHalfDown) {
    EXPECT_EQ(runI32("return (int32) Math.round(3.4);"), 3);
}

TEST(MathIntrinsicTests, piConstant) {
    EXPECT_DOUBLE_EQ(runF64("return Math.PI;"), 3.141592653589793);
}

TEST(MathIntrinsicTests, eConstant) {
    EXPECT_DOUBLE_EQ(runF64("return Math.E;"), 2.718281828459045);
}

TEST(MathIntrinsicTests, integerMaxValue) {
    EXPECT_EQ(runI32("return Integer.MAX_VALUE;"), 2147483647);
}

TEST(MathIntrinsicTests, integerMinValue) {
    // Don't use INT32_MIN directly — that'd be -INT32_MAX-1 and we want the runtime
    // to confirm the constant matches exactly.
    EXPECT_EQ(runI32(
        "if (Integer.MIN_VALUE == -2147483648) return 1;\n"
        "return 0;"), 1);
}

TEST(MathIntrinsicTests, sinZero) {
    EXPECT_NEAR(runF64("return Math.sin(0.0);"), 0.0, 1e-12);
}

TEST(MathIntrinsicTests, cosZero) {
    EXPECT_DOUBLE_EQ(runF64("return Math.cos(0.0);"), 1.0);
}

TEST(MathIntrinsicTests, log10Hundred) {
    EXPECT_DOUBLE_EQ(runF64("return Math.log10(100.0);"), 2.0);
}

TEST(MathIntrinsicTests, expZero) {
    EXPECT_DOUBLE_EQ(runF64("return Math.exp(0.0);"), 1.0);
}

TEST(MathIntrinsicTests, tanZero) {
    EXPECT_NEAR(runF64("return Math.tan(0.0);"), 0.0, 1e-12);
}
