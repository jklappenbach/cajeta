//
// Binary-op tests: arithmetic / bitwise / shift across int and float, exercised
// via JIT execution of compiled Cajeta source.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class B {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- int128: does 128-bit math actually work end-to-end? ----------------------
//
// int128/uint128 map to LLVM i128 and x86-64 has __int128, but there was no JIT
// exec coverage. These probe REAL 128-bit behavior (a result that needs the high
// 64 bits), not just that the keyword parses.

// Store + widen + narrow round-trip (low word only).
TEST(BinaryOpTests, int128WidenNarrow) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int128 a = (int128) 1000L;\n"
        "return (int64) a;"), "test.B");
    EXPECT_EQ(jit->lookup<int64_t (*)()>("run")(), 1000);
}

// Multiply two ~1e10 values → 1e20, which OVERFLOWS int64. The high 64 bits must
// be nonzero (floor(1e20 / 2^64) == 5). A 64-bit-only multiply would yield 0
// here — so this distinguishes true i128 from a truncated path.
TEST(BinaryOpTests, int128MulNeedsHighWord) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int128 a = (int128) 10000000000L;\n"   // 1e10
        "int128 b = a * a;\n"                    // 1e20, needs 67 bits
        "return (int64) (b >> 64);"), "test.B");
    EXPECT_EQ(jit->lookup<int64_t (*)()>("run")(), 5);
}

// Shift a 1 up past the 64-bit boundary and back down.
TEST(BinaryOpTests, int128ShiftAcross64) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int128 a = (int128) 1L;\n"
        "a = a << 100;\n"
        "return (int64) (a >> 100);"), "test.B");
    EXPECT_EQ(jit->lookup<int64_t (*)()>("run")(), 1);
}

// 128-bit DIVISION — lowers to the __divti3 compiler-rt libcall; this tells us
// whether that symbol resolves in the JIT.
TEST(BinaryOpTests, int128Division) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int128 a = (int128) 10000000000L;\n"   // 1e10
        "int128 b = a * a;\n"                    // 1e20
        "int128 c = b / a;\n"                    // back to 1e10
        "return (int64) c;"), "test.B");
    EXPECT_EQ(jit->lookup<int64_t (*)()>("run")(), 10000000000LL);
}

// Unsigned 128-bit multiply high word (uses __udivti3 path for the >> nothing,
// but exercises uint128 mul + logical shift).
TEST(BinaryOpTests, uint128MulNeedsHighWord) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "uint128 a = (uint128) 10000000000L;\n"
        "uint128 b = a * a;\n"
        "return (int64) (b >> 64);"), "test.B");
    EXPECT_EQ(jit->lookup<int64_t (*)()>("run")(), 5);
}

// --- integer arithmetic --------------------------------------------------------

TEST(BinaryOpTests, intAdd) {
    auto jit = CajetaJit::compile(makeSource("int32", "return 2 + 3;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 5);
}

TEST(BinaryOpTests, intSubtract) {
    auto jit = CajetaJit::compile(makeSource("int32", "return 10 - 4;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 6);
}

TEST(BinaryOpTests, intMultiply) {
    auto jit = CajetaJit::compile(makeSource("int32", "return 6 * 7;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

TEST(BinaryOpTests, intDivideSigned) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 20;\n"
        "int32 b = 6;\n"
        "return a / b;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

TEST(BinaryOpTests, intMod) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 20;\n"
        "int32 b = 6;\n"
        "return a % b;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

TEST(BinaryOpTests, floatMod) {
    auto jit = CajetaJit::compile(makeSource("float64",
        "float64 a = 10.5;\n"
        "float64 b = 3.0;\n"
        "return a % b;"), "test.B");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 1.5);
}

TEST(BinaryOpTests, intAddViaLocals) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 10;\n"
        "int32 b = 32;\n"
        "return a + b;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// --- compound assignment -------------------------------------------------------

TEST(BinaryOpTests, addEquals) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 10;\n"
        "a += 5;\n"
        "return a;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 15);
}

TEST(BinaryOpTests, subEquals) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 10;\n"
        "a -= 3;\n"
        "return a;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

TEST(BinaryOpTests, mulEquals) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 6;\n"
        "a *= 7;\n"
        "return a;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// --- bitwise -------------------------------------------------------------------

TEST(BinaryOpTests, bitAnd) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 12;\n"  // 1100
        "int32 b = 10;\n"  // 1010
        "return a & b;"),
        "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 8);  // 1000
}

TEST(BinaryOpTests, bitOr) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 12;\n"
        "int32 b = 10;\n"
        "return a | b;"),
        "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 14);  // 1110
}

TEST(BinaryOpTests, bitXor) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 12;\n"
        "int32 b = 10;\n"
        "return a ^ b;"),
        "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 6);  // 0110
}

TEST(BinaryOpTests, shiftLeft) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 1;\n"
        "int32 b = 4;\n"
        "return a << b;"),
        "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 16);
}

TEST(BinaryOpTests, shiftRightSigned) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 32;\n"
        "int32 b = 2;\n"
        "return a >> b;"),
        "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 8);
}

// --- floating-point arithmetic -------------------------------------------------

TEST(BinaryOpTests, floatAdd) {
    auto jit = CajetaJit::compile(makeSource("float32",
        "float32 a = 1.5f;\n"
        "float32 b = 2.25f;\n"
        "return a + b;"),
        "test.B");
    auto fn = jit->lookup<float (*)()>("run");
    EXPECT_FLOAT_EQ(fn(), 3.75f);
}

TEST(BinaryOpTests, doubleSubtract) {
    auto jit = CajetaJit::compile(makeSource("float64",
        "float64 a = 10.5;\n"
        "float64 b = 4.25;\n"
        "return a - b;"),
        "test.B");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 6.25);
}

TEST(BinaryOpTests, doubleMultiply) {
    auto jit = CajetaJit::compile(makeSource("float64",
        "float64 a = 3.0;\n"
        "float64 b = 4.0;\n"
        "return a * b;"),
        "test.B");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 12.0);
}

TEST(BinaryOpTests, doubleDivide) {
    auto jit = CajetaJit::compile(makeSource("float64",
        "float64 a = 15.0;\n"
        "float64 b = 4.0;\n"
        "return a / b;"),
        "test.B");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 3.75);
}
