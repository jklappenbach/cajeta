//
// Prefix, postfix, and cast expression tests.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class U {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- prefix --------------------------------------------------------------------

TEST(PrefixTests, unaryMinusInt) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 5;\n"
        "return -a;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), -5);
}

TEST(PrefixTests, unaryMinusFloat) {
    auto jit = CajetaJit::compile(makeSource("float64",
        "float64 a = 3.14;\n"
        "return -a;"), "test.U");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), -3.14);
}

TEST(PrefixTests, unaryPlusIdentity) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 7;\n"
        "return +a;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

TEST(PrefixTests, bitwiseNot) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 0;\n"
        "return ~a;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), -1);  // ~0 = -1 in signed two's complement
}

TEST(PrefixTests, logicalNotFalse) {
    auto jit = CajetaJit::compile(makeSource("boolean",
        "boolean b = false;\n"
        "return !b;"), "test.U");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(PrefixTests, logicalNotTrue) {
    auto jit = CajetaJit::compile(makeSource("boolean",
        "boolean b = true;\n"
        "return !b;"), "test.U");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_FALSE(fn());
}

TEST(PrefixTests, preIncrementReturnsNew) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 5;\n"
        "int32 b = ++a;\n"
        "return a + b;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // a becomes 6, b is the new value 6 → 6 + 6 = 12
    EXPECT_EQ(fn(), 12);
}

TEST(PrefixTests, preDecrementReturnsNew) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 5;\n"
        "int32 b = --a;\n"
        "return a + b;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // a becomes 4, b is the new value 4 → 4 + 4 = 8
    EXPECT_EQ(fn(), 8);
}

// --- postfix -------------------------------------------------------------------

TEST(PostfixTests, postIncrementReturnsOld) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 5;\n"
        "int32 b = a++;\n"
        "return a + b;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // a becomes 6, b is the old value 5 → 6 + 5 = 11
    EXPECT_EQ(fn(), 11);
}

TEST(PostfixTests, postDecrementReturnsOld) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 5;\n"
        "int32 b = a--;\n"
        "return a + b;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // a becomes 4, b is the old value 5 → 4 + 5 = 9
    EXPECT_EQ(fn(), 9);
}

// --- cast ----------------------------------------------------------------------

TEST(CastTests, intToFloat) {
    auto jit = CajetaJit::compile(makeSource("float32",
        "int32 a = 5;\n"
        "return (float32) a;"), "test.U");
    auto fn = jit->lookup<float (*)()>("run");
    EXPECT_FLOAT_EQ(fn(), 5.0f);
}

TEST(CastTests, floatToInt) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "float64 a = 3.7;\n"
        "return (int32) a;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);  // truncation toward zero
}

TEST(CastTests, intWiden) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int32 a = 42;\n"
        "return (int64) a;"), "test.U");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

TEST(CastTests, intNarrow) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int64 a = 1000;\n"
        "return (int32) a;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1000);
}

TEST(CastTests, doubleToFloat) {
    auto jit = CajetaJit::compile(makeSource("float32",
        "float64 a = 1.5;\n"
        "return (float32) a;"), "test.U");
    auto fn = jit->lookup<float (*)()>("run");
    EXPECT_FLOAT_EQ(fn(), 1.5f);
}

TEST(CastTests, floatToDouble) {
    auto jit = CajetaJit::compile(makeSource("float64",
        "float32 a = 2.25f;\n"
        "return (float64) a;"), "test.U");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 2.25);
}

// Integer WIDENING follows the SOURCE operand's signedness, not the
// destination's. Regression for the bug where (int32) of a uint8 200 used the
// destination (signed int32) and sign-extended to -56.
TEST(CastTests, unsignedByteWidensZeroExtended) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "uint8 a = (uint8) 200;\n"
        "return (int32) a;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 200);
}

TEST(CastTests, unsignedShortWidensZeroExtended) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "uint16 a = (uint16) 60000;\n"
        "return (int32) a;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 60000);
}

TEST(CastTests, unsignedIntWidensToInt64ZeroExtended) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "uint32 a = (uint32) 4000000000L;\n"   // > 2^31; sign-extend would go negative
        "return (int64) a;"), "test.U");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 4000000000LL);
}

// Signed widening still sign-extends (the case that already worked).
TEST(CastTests, signedByteWidensSignExtended) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int8 a = (int8) -5;\n"
        "return (int32) a;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), -5);
}

// int→fp also keys off the source: a large uint32 converts via UIToFP.
TEST(CastTests, unsignedIntToFloatUsesUnsignedConversion) {
    auto jit = CajetaJit::compile(makeSource("float64",
        "uint32 a = (uint32) 4000000000L;\n"
        "return (float64) a;"), "test.U");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 4000000000.0);
}
