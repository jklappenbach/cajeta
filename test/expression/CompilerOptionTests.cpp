//
// Tests for compiler-level options: --bounds=off, --emit=obj.
// --emit=exe requires lld libraries; covered by integration tests, not here.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../DeathTestUtil.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>

using cajeta_test::CajetaJit;
using cajeta::Compiler;
using cajeta::EmitMode;

namespace {

std::string arraySource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class O {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// With bounds-check disabled, an out-of-range index does not call the abort helper —
// the GEP runs and returns whatever the buffer has past the end (garbage). The
// observable signal is "doesn't abort"; we use a small-enough out-of-range read so
// it stays within the calloc'd chunk on glibc malloc (avoiding a real segfault).

// Verify the EmitMode setter / getter contract.

// Verify the target triple setter changes the TargetMachine appropriately.

// --ub-traps=on (the default under --debug): divide-by-zero traps
// via @llvm.trap (SIGILL on x86) before the SDiv would execute.
TEST(CompilerOptionTests, ubTrapsDivideByZeroTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 10;\n"
        "        int32 b = 0;\n"
        "        return a / b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --ub-traps=on: modulo-by-zero traps the same way (SRem on rhs=0
// is the same UB shape as SDiv).
TEST(CompilerOptionTests, ubTrapsModByZeroTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 10;\n"
        "        int32 b = 0;\n"
        "        return a % b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --ub-traps=on: shift count >= operand bit-width traps. On int32,
// any count >= 32 (or < 0) is UB per LLVM's IR semantics.
TEST(CompilerOptionTests, ubTrapsOversizedShiftTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 1;\n"
        "        int32 b = 64;\n"  // way past int32's 32-bit width
        "        return a << b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --ub-traps=on: in-range division still works. Sanity that the
// guard branch doesn't fire on legitimate inputs.
TEST(CompilerOptionTests, ubTrapsValidDivisionWorks) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 21;\n"
        "        int32 b = 3;\n"
        "        return a / b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// --overflow-checks=on (default): signed add at int32 max + 1 traps.
TEST(CompilerOptionTests, overflowChecksSignedAddTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 2147483647;\n"  // INT32_MAX
        "        int32 b = 1;\n"
        "        return a + b;\n"           // wraps without check, traps with
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: signed sub at int32 min - 1 traps.
TEST(CompilerOptionTests, overflowChecksSignedSubTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = -2147483648;\n"  // INT32_MIN
        "        int32 b = 1;\n"
        "        return a - b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: signed mul int32 max * 2 traps.
TEST(CompilerOptionTests, overflowChecksSignedMulTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 1073741824;\n"  // 2^30
        "        int32 b = 4;\n"            // 2^30 * 4 == 2^32 overflows
        "        return a * b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: in-range arithmetic still produces correct results.
TEST(CompilerOptionTests, overflowChecksValidArithmeticWorks) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 100;\n"
        "        int32 b = 5;\n"
        "        return (a + b) * (a - b);\n"  // 105 * 95 == 9975
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 9975);
}

// --overflow-checks=on: uint64 arithmetic that would signed-overflow
// must NOT trap (the operand type is unsigned, so wrapping is the
// defined behavior). This pins that the cast's resolvedType properly
// propagates the unsigned signedness, so the overflow guard rule
// (which only fires on SIGNED_FLAG) skips the check.
TEST(CompilerOptionTests, overflowChecksUnsignedMulDoesNotTrap) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int64 run() {\n"
        "        uint64 a = (uint64) 0xCBF29CE484222325;\n"
        "        uint64 b = (uint64) 0x100000001B3;\n"
        "        return (int64) (a * b);\n"  // would signed-overflow; OK as uint
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // 0xCBF29CE484222325 * 0x100000001B3 wraps to a specific uint64
    // value; just verify it runs without trapping.
    uint64_t a = 0xCBF29CE484222325ULL;
    uint64_t b = 0x100000001B3ULL;
    int64_t expected = (int64_t) (a * b);
    EXPECT_EQ(fn(), expected);
}

// Mirror String.hash's structure: while loop with uint64 var,
// XOR with uint64 literal, then multiply by uint64 literal.
TEST(CompilerOptionTests, overflowChecksUnsignedFnvShapeLoop) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int64 run() {\n"
        "        uint64 h = (uint64) 0xCBF29CE484222325;\n"
        "        int32 i = 0;\n"
        "        while (i < 3) {\n"
        "            uint64 b = (uint64) i;\n"
        "            h = h ^ b;\n"
        "            h = h * (uint64) 0x100000001B3;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return (int64) h;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    uint64_t h = 0xCBF29CE484222325ULL;
    for (int32_t i = 0; i < 3; ++i) {
        h ^= (uint64_t) i;
        h *= 0x100000001B3ULL;
    }
    EXPECT_EQ(fn(), (int64_t) h);
}

// Pin the FNV-1a-shape: uint64 var, XOR with uint64 literal, then
// multiply by uint64 literal — all under overflowChecks=on. This is
// the shape String.hash() and other hashers use. The XOR previously
// poisoned the lvalue's resolvedType so the subsequent multiply was
// treated as signed and trapped.
TEST(CompilerOptionTests, overflowChecksUnsignedFnvShape) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int64 run() {\n"
        "        uint64 h = (uint64) 0xCBF29CE484222325;\n"
        "        uint64 b = (uint64) 99;\n"
        "        h = h ^ b;\n"
        "        h = h * (uint64) 0x100000001B3;\n"
        "        return (int64) h;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    uint64_t h = 0xCBF29CE484222325ULL;
    h ^= 99ULL;
    h *= 0x100000001B3ULL;
    EXPECT_EQ(fn(), (int64_t) h);
}

// --overflow-checks=on: compound += traps on signed overflow.
TEST(CompilerOptionTests, overflowChecksCompoundAddTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 2147483647;\n"
        "        a += 1;\n"
        "        return a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: compound -= traps.
TEST(CompilerOptionTests, overflowChecksCompoundSubTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = -2147483648;\n"
        "        a -= 1;\n"
        "        return a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: compound *= traps.
TEST(CompilerOptionTests, overflowChecksCompoundMulTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 1073741824;\n"
        "        a *= 4;\n"
        "        return a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: compound assignment on uint32 wraps silently.
TEST(CompilerOptionTests, overflowChecksCompoundUnsignedDoesNotTrap) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        uint32 a = (uint32) 4294967295;\n"  // UINT32_MAX
        "        a += (uint32) 1;\n"
        "        return (int32) a;\n"                 // wraps to 0
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// --overflow-checks=on: unary -INT_MIN traps (the one int the
// signed range can't negate).
TEST(CompilerOptionTests, overflowChecksUnaryNegIntMinTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = -2147483648;\n"  // INT32_MIN
        "        return -a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: normal negation works.
TEST(CompilerOptionTests, overflowChecksUnaryNegNormalWorks) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 42;\n"
        "        return -a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), -42);
}

// --overflow-checks=on: unsigned negation doesn't check (unsigned
// arithmetic is modular by definition).
TEST(CompilerOptionTests, overflowChecksUnaryNegUnsignedDoesNotTrap) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        uint32 a = (uint32) 1;\n"
        "        uint32 b = -a;\n"  // wraps to UINT32_MAX; OK as uint
        "        return (int32) b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), -1);
}

// --overflow-checks=on: prefix ++ traps at INT_MAX.
TEST(CompilerOptionTests, overflowChecksPrefixIncTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 2147483647;\n"
        "        ++a;\n"
        "        return a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: prefix -- traps at INT_MIN.
TEST(CompilerOptionTests, overflowChecksPrefixDecTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = -2147483648;\n"
        "        --a;\n"
        "        return a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: postfix ++ traps at INT_MAX.
TEST(CompilerOptionTests, overflowChecksPostfixIncTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 2147483647;\n"
        "        a++;\n"
        "        return a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: postfix -- traps at INT_MIN.
TEST(CompilerOptionTests, overflowChecksPostfixDecTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = -2147483648;\n"
        "        a--;\n"
        "        return a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --overflow-checks=on: ++ / -- on uint*, modular wrap, no trap.
TEST(CompilerOptionTests, overflowChecksIncDecUnsignedDoesNotTrap) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        uint32 a = (uint32) 4294967295;\n"  // UINT32_MAX
        "        a++;\n"                              // wraps to 0
        "        return (int32) a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// --overflow-checks=on: normal ++/-- on int32 (no overflow) work.
TEST(CompilerOptionTests, overflowChecksIncDecNormalWorks) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 5;\n"
        "        ++a;\n"
        "        a++;\n"
        "        --a;\n"
        "        return a;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.O");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 6);
}

// --bounds=trap: out-of-bounds index traps via @llvm.trap instead of
// calling the abort-helper. Observable as SIGILL rather than SIGABRT
// with the "out of bounds" message.
TEST(CompilerOptionTests, boundsCheckTrapVariantTraps) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = heap int32[3];\n"
        "        return arr[5];\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.boundsCheckMode = cajeta::BoundsCheck::Trap;
    auto jit = CajetaJit::compile(src, "test.O", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_TRAP, "");
}

// --live-set=off: __cajeta_live_set_add is not called after malloc,
// so the live-set runtime structure remains empty. Observable via
// the runtime function existing but not being invoked.
TEST(CompilerOptionTests, liveSetOffSkipsRegistration) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public int32 v;\n"
        "    public O() { this.v = 7; }\n"
        "    public static int32 run() {\n"
        "        O o = heap O();\n"
        "        return o.v;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.liveSetMode = cajeta::LiveSet::Off;
    auto jit = CajetaJit::compile(src, "test.O", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 7);
}

// --overflow-checks=off: signed overflow wraps silently.
TEST(CompilerOptionTests, overflowChecksOffWrapsSilently) {
    auto src =
        "package test;\n"
        "public final class O {\n"
        "    public static int32 run() {\n"
        "        int32 a = 2147483647;\n"  // INT32_MAX
        "        int32 b = 1;\n"
        "        return a + b;\n"           // expected: -2147483648 (wrap)
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.overflowChecksEnabled = false;
    auto jit = CajetaJit::compile(src, "test.O", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), INT32_MIN);
}
