//
// Integer signedness in binary-op codegen.
//
// THE ROOT CAUSE these pin: signedness was read from the llvm::Value via
// `CajetaType::getTypeFlagsOf`, which looks up `llvmTypeIdMap` — a map keyed
// by `llvm::Type::TypeID`. Every integer width shares `IntegerTyID`, so that
// map holds ONE entry covering int8..uint64 and whichever integer type
// registered last decided what every integer value appeared to be. Anything
// branching on SIGNED_FLAG therefore answered a type-system question with a
// fact about LLVM's type IDs.
//
// Three consequences, all silent — wrong numbers, never a crash:
//   * `>>` emitted `ashr` unconditionally, so a uint64 shifted in ones;
//   * `/` picked `udiv` for signed operands, so int64 -8 / 2 evaluated to
//     9223372036854775804;
//   * a narrower unsigned operand widened SIGN-extended, so uint32 max
//     became a 64-bit run of ones before the operation.
//
// The fix takes signedness from the AST's resolved type (which knows int64
// from uint64) and keeps the value-derived flags only as a fallback.
//
// Note on `>>`: the fill bit follows the SHIFTED operand alone. The right
// operand is a count, and folding its signedness in — as `/` legitimately
// does with `lhs | rhs` — would make `someUint64 >> 33` arithmetic, because
// the literal count is a signed int32.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int64_t runI64(const std::string& body) {
    auto src = "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n" + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

// `>>` on an unsigned operand is LOGICAL: the high bit must not replicate.
TEST(IntegerSignednessTests, unsignedShiftRightIsLogical) {
    EXPECT_EQ(runI64(
        "        uint64 x = (uint64) 18446744073709551615;\n"
        "        return (int64) (x >> 33);\n"), 2147483647);
    // ...and the compound form takes the same path.
    EXPECT_EQ(runI64(
        "        uint64 x = (uint64) 18446744073709551615;\n"
        "        x = x >> 33;\n"
        "        return (int64) x;\n"), 2147483647);
    // A narrower unsigned operand must widen ZERO-extended, or it becomes a
    // 64-bit run of ones before the shift ever happens.
    EXPECT_EQ(runI64(
        "        uint32 x = (uint32) 4294967295;\n"
        "        return (int64) (x >> 16);\n"), 65535);
}

// `>>` on a signed operand stays ARITHMETIC — the fix must not simply
// flip the instruction.
TEST(IntegerSignednessTests, signedShiftRightIsArithmetic) {
    EXPECT_EQ(runI64(
        "        int64 x = (int64) -1;\n"
        "        return x >> 33;\n"), -1);
    EXPECT_EQ(runI64(
        "        int64 x = (int64) -1;\n"
        "        x = x >> 33;\n"
        "        return x;\n"), -1);
    EXPECT_EQ(runI64(
        "        int32 x = (int32) -8;\n"
        "        return (int64) (x >> 1);\n"), -4);
}

// Division follows the operands' signedness. int64 signed division was
// emitting `udiv` before the fix; int32 happened to be correct, which is
// exactly the kind of width-dependent accident a shared TypeID map produces.
TEST(IntegerSignednessTests, divisionFollowsSignedness) {
    EXPECT_EQ(runI64(
        "        int64 a = (int64) -8;\n"
        "        return a / (int64) 2;\n"), -4);
    EXPECT_EQ(runI64(
        "        int32 a = (int32) -8;\n"
        "        return (int64) (a / (int32) 2);\n"), -4);
    // Unsigned division of a value above 2^63 must NOT go signed: as a
    // signed quantity this dividend is -2, and -2 / 2 = -1 (all ones).
    // Returned directly — 2^63-1 is representable in int64, so no ternary
    // or comparison sits between the division and the assertion.
    EXPECT_EQ(runI64(
        "        uint64 a = (uint64) 18446744073709551614;\n"
        "        uint64 q = a / (uint64) 2;\n"
        "        return (int64) q;\n"),
        9223372036854775807LL);
}

// Modulo, the sibling of division, on both signs.
TEST(IntegerSignednessTests, moduloFollowsSignedness) {
    EXPECT_EQ(runI64(
        "        int64 a = (int64) -8;\n"
        "        return a % (int64) 3;\n"), -2);
    EXPECT_EQ(runI64(
        "        uint64 a = (uint64) 18446744073709551615;\n"
        "        uint64 m = a % (uint64) 10;\n"
        "        return (int64) m;\n"), 5);
}

// A uint64 above 2^63 has no signed rendering: formatting it through the
// int64 path printed it negative while `==` against the unsigned literal
// still passed — so a value could compare right and print wrong.
TEST(IntegerSignednessTests, unsignedValuesRenderUnsigned) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        uint64 x = (uint64) 18446744073709551615;\n"
        "        String s = \"\" + x;\n"
        "        if (s.equals(\"18446744073709551615\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
    // The signed rendering is untouched.
    auto jit2 = CajetaJit::compile(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 x = (int64) -5808556873153909620;\n"
        "        String s = \"\" + x;\n"
        "        if (s.equals(\"-5808556873153909620\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn2 = jit2->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn2(), 1);
}

// The end-to-end shape that surfaced this: murmur3's fmix64, which is pure
// unsigned shift/multiply/xor. Its published vector for input 1 only comes
// out right if every step is unsigned.
TEST(IntegerSignednessTests, fmix64MatchesTheReferenceVector) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public final class D {\n"
        "    static uint64 fmix(uint64 h) {\n"
        "        uint64 x = h;\n"
        "        x = x ^ (x >> 33);\n"
        "        x = x * (uint64) 18397679294719823053;\n"
        "        x = x ^ (x >> 33);\n"
        "        x = x * (uint64) 14181476777654086739;\n"
        "        x = x ^ (x >> 33);\n"
        "        return x;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        if (D.fmix((uint64) 1) == (uint64) 12994781566227106604) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
