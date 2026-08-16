//
// Tests for Integer.parseInt, Long.parseLong, Double.parseDouble,
// Boolean.parseBoolean, *.toString(...) and String.valueOf(...).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body, const std::string& retType = "int32") {
    return "package test;\n"
           "public final class Cv {\n"
           "    public static " + retType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runI32(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body, "int32"), "test.Cv");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

double runF64(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body, "float64"), "test.Cv");
    auto fn = jit->lookup<double (*)()>("run");
    return fn();
}

} // namespace








TEST(ConversionIntrinsicTests, parseBooleanCaseInsensitive) {
    EXPECT_EQ(runI32(
        "if (Boolean.parseBoolean(\"TRUE\")) return 1;\n"
        "return 0;"), 1);
}


TEST(ConversionIntrinsicTests, doubleToStringMatchesValue) {
    EXPECT_EQ(runI32(
        "String s = Double.toString(2.5);\n"
        "if (s.equals(\"2.5\")) return 1;\n"
        "return 0;"), 1);
}

TEST(ConversionIntrinsicTests, booleanToStringTrue) {
    EXPECT_EQ(runI32(
        "String s = Boolean.toString(true);\n"
        "if (s.equals(\"true\")) return 1;\n"
        "return 0;"), 1);
}

TEST(ConversionIntrinsicTests, stringValueOfInt) {
    EXPECT_EQ(runI32(
        "String s = String.valueOf(7);\n"
        "if (s.equals(\"7\")) return 1;\n"
        "return 0;"), 1);
}

TEST(ConversionIntrinsicTests, stringValueOfBool) {
    EXPECT_EQ(runI32(
        "String s = String.valueOf(false);\n"
        "if (s.equals(\"false\")) return 1;\n"
        "return 0;"), 1);
}

TEST(ConversionIntrinsicTests, stringValueOfChar) {
    EXPECT_EQ(runI32(
        "String s = String.valueOf('Q');\n"
        "if (s.equals(\"Q\")) return 1;\n"
        "return 0;"), 1);
}

TEST(ConversionIntrinsicTests, roundTripIntStringInt) {
    EXPECT_EQ(runI32(
        "return Integer.parseInt(Integer.toString(2026));"), 2026);
}

// --- bit-twiddling intrinsics -----------------------------------------------

TEST(ConversionIntrinsicTests, integerBitCount) {
    // popcount(0b1011 = 11) = 3
    EXPECT_EQ(runI32("return Integer.bitCount(11);"), 3);
}


TEST(ConversionIntrinsicTests, integerNumberOfLeadingZeros) {
    // 1 has 31 leading zeros in a 32-bit value.
    EXPECT_EQ(runI32("return Integer.numberOfLeadingZeros(1);"), 31);
}

TEST(ConversionIntrinsicTests, integerNumberOfTrailingZeros) {
    // 8 == 0b1000 → 3 trailing zeros.
    EXPECT_EQ(runI32("return Integer.numberOfTrailingZeros(8);"), 3);
}



