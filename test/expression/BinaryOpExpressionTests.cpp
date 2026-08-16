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

// Multiply two ~1e10 values → 1e20, which OVERFLOWS int64. The high 64 bits must
// be nonzero (floor(1e20 / 2^64) == 5). A 64-bit-only multiply would yield 0
// here — so this distinguishes true i128 from a truncated path.

// Shift a 1 up past the 64-bit boundary and back down.

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

// --- integer arithmetic --------------------------------------------------------








// --- compound assignment -------------------------------------------------------



TEST(BinaryOpTests, mulEquals) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 6;\n"
        "a *= 7;\n"
        "return a;"), "test.B");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// --- bitwise -------------------------------------------------------------------






// --- floating-point arithmetic -------------------------------------------------




