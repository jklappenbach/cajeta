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

TEST(ConversionIntrinsicTests, parseIntPositive) {
    EXPECT_EQ(runI32("return Integer.parseInt(\"42\");"), 42);
}

TEST(ConversionIntrinsicTests, parseIntNegative) {
    EXPECT_EQ(runI32("return Integer.parseInt(\"-123\");"), -123);
}

TEST(ConversionIntrinsicTests, parseIntInvalidReturnsZero) {
    EXPECT_EQ(runI32("return Integer.parseInt(\"abc\");"), 0);
}

TEST(ConversionIntrinsicTests, parseLongLarge) {
    EXPECT_EQ(runI32(
        "int64 v = Long.parseLong(\"9999999999\");\n"
        "if (v == 9999999999) return 1;\n"
        "return 0;"), 1);
}

TEST(ConversionIntrinsicTests, parseDoubleBasic) {
    EXPECT_DOUBLE_EQ(runF64("return Double.parseDouble(\"3.14\");"), 3.14);
}

TEST(ConversionIntrinsicTests, parseBooleanTrue) {
    EXPECT_EQ(runI32(
        "if (Boolean.parseBoolean(\"true\")) return 1;\n"
        "return 0;"), 1);
}

TEST(ConversionIntrinsicTests, parseBooleanFalse) {
    EXPECT_EQ(runI32(
        "if (Boolean.parseBoolean(\"false\")) return 1;\n"
        "return 0;"), 0);
}

TEST(ConversionIntrinsicTests, parseBooleanCaseInsensitive) {
    EXPECT_EQ(runI32(
        "if (Boolean.parseBoolean(\"TRUE\")) return 1;\n"
        "return 0;"), 1);
}

TEST(ConversionIntrinsicTests, integerToStringMatchesValue) {
    EXPECT_EQ(runI32(
        "String s = Integer.toString(99);\n"
        "if (s.equals(\"99\")) return 1;\n"
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

TEST(ConversionIntrinsicTests, integerBitCountZero) {
    EXPECT_EQ(runI32("return Integer.bitCount(0);"), 0);
}

TEST(ConversionIntrinsicTests, integerNumberOfLeadingZeros) {
    // 1 has 31 leading zeros in a 32-bit value.
    EXPECT_EQ(runI32("return Integer.numberOfLeadingZeros(1);"), 31);
}

TEST(ConversionIntrinsicTests, integerNumberOfTrailingZeros) {
    // 8 == 0b1000 → 3 trailing zeros.
    EXPECT_EQ(runI32("return Integer.numberOfTrailingZeros(8);"), 3);
}

TEST(ConversionIntrinsicTests, integerNumberOfLeadingZerosOfZero) {
    // Java's behavior: 32 when the input is 0.
    EXPECT_EQ(runI32("return Integer.numberOfLeadingZeros(0);"), 32);
}

TEST(ConversionIntrinsicTests, longBitCountMatchesValue) {
    // 0xFFFFFFFF as int64 has 32 set bits.
    EXPECT_EQ(runI32("return Integer.bitCount(255);"), 8);
}

TEST(ConversionIntrinsicTests, integerReverseRoundTrip) {
    // Reversing twice must yield the original value.
    EXPECT_EQ(runI32(
        "int32 x = 305419896;\n"  // 0x12345678
        "int32 y = Integer.reverse(Integer.reverse(x));\n"
        "if (x == y) return 1;\n"
        "return 0;"), 1);
}
