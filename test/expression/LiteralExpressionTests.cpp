//
// Literal expression tests: integer / float / bool / string literals are compiled
// into trivial methods that return the literal, JIT'd, and the returned value is
// asserted against the expected constant.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <cstring>

using cajeta_test::CajetaJit;

namespace {

// Smallest possible source — a static method that returns a literal.
std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class L {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

TEST(LiteralExpressionTests, integerLiteralDecimal) {
    auto jit = CajetaJit::compile(makeSource("int32", "return 42;"), "test.L");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

TEST(LiteralExpressionTests, integerLiteralZero) {
    auto jit = CajetaJit::compile(makeSource("int32", "return 0;"), "test.L");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

TEST(LiteralExpressionTests, integerLiteralLarge) {
    // Verify the bit-width selection in IntegerLiteralExpression scales to int64.
    auto jit = CajetaJit::compile(makeSource("int64", "return 1234567890;"), "test.L");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1234567890LL);
}

// L-suffix pins the literal's Cajeta type to int64. Without the
// suffix-aware strip, APInt(64, "8L", 10) misreads the non-digit `L`
// and produces garbage (surfaced as `8L` → 79 during method-template
// testing).
TEST(LiteralExpressionTests, integerLiteralLongSuffix) {
    auto jit = CajetaJit::compile(makeSource("int64", "return 8L;"), "test.L");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 8LL);
}

TEST(LiteralExpressionTests, integerLiteralLongSuffixBig) {
    // Value larger than int32 range exercises the L-suffix branch end-to-end.
    auto jit = CajetaJit::compile(
        makeSource("int64", "return 5000000000L;"), "test.L");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 5000000000LL);
}

TEST(LiteralExpressionTests, integerLiteralLowercaseLSuffix) {
    auto jit = CajetaJit::compile(makeSource("int64", "return 42l;"), "test.L");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42LL);
}

// Digit-separator underscores (`1_000_000`) are stripped before APInt
// parses; the lexer accepts them per Java's syntax.
TEST(LiteralExpressionTests, integerLiteralUnderscoreSeparator) {
    auto jit = CajetaJit::compile(
        makeSource("int32", "return 1_000_000;"), "test.L");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1000000);
}

// Hex literal with L suffix.
TEST(LiteralExpressionTests, integerLiteralHexLongSuffix) {
    auto jit = CajetaJit::compile(
        makeSource("int64", "return 0xFFL;"), "test.L");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0xFFLL);
}

// Binary literal.
TEST(LiteralExpressionTests, integerLiteralBinary) {
    auto jit = CajetaJit::compile(
        makeSource("int32", "return 0b1010;"), "test.L");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 10);
}

TEST(LiteralExpressionTests, floatLiteralFloat32) {
    auto jit = CajetaJit::compile(makeSource("float32", "return 1.5f;"), "test.L");
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 1.5f);
}

TEST(LiteralExpressionTests, floatLiteralFloat64) {
    auto jit = CajetaJit::compile(makeSource("float64", "return 3.14159;"), "test.L");
    auto fn = jit->lookup<double (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_DOUBLE_EQ(fn(), 3.14159);
}

TEST(LiteralExpressionTests, floatLiteralNegativeSign) {
    // Prefix unary `-` applied to a positive float literal.
    auto jit = CajetaJit::compile(makeSource("float64", "return -2.5;"), "test.L");
    auto fn = jit->lookup<double (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_DOUBLE_EQ(fn(), -2.5);
}

TEST(LiteralExpressionTests, boolLiteralTrue) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return true;"), "test.L");
    auto fn = jit->lookup<bool (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn());
}

TEST(LiteralExpressionTests, boolLiteralFalse) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return false;"), "test.L");
    auto fn = jit->lookup<bool (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn());
}
