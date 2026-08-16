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



TEST(PrefixTests, unaryPlusIdentity) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 7;\n"
        "return +a;"), "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}






// --- postfix -------------------------------------------------------------------



// --- cast ----------------------------------------------------------------------







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

// --- cast of a parenthesized operand -------------------------------------------
//
// `(E) (expr)` — a cast whose destination names a TYPE and whose operand is
// PARENTHESIZED — is captured by the postfix-call alternative of the
// left-recursive `expression` rule: `(E)` is itself a valid parenthesized
// identifier expression, so `(E)(x)` matches `expression '(' args ')'` before
// the cast alternative is tried. A primitive destination cannot match that
// (`(int64)` is not an expression), which is why primitives always worked.
//
// See specs/typeparam-cast-of-paren-spec.md. The dominant idiom this breaks is
// generic numeric code: "reduce in float64, narrow to E on store".

// 1 — the reported case: a type-parameter destination over a parenthesized
// arithmetic operand, inside a generic method.
TEST(CastTests, typeParamCastOfParenthesizedOperand) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public final class U {\n"
        "    static E narrow<E extends Floating>(float64 acc) {\n"
        "        return (E) (acc / 2.0);\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        return U.narrow<float32>(9.0);\n"
        "    }\n"
        "}\n", "test.U");
    auto fn = jit->lookup<float (*)()>("run");
    EXPECT_FLOAT_EQ(fn(), 4.5f);
}

// 2 — a CLASS-level type parameter, same shape (the ml.grad GradTape<E> case).
TEST(CastTests, classTypeParamCastOfParenthesizedOperand) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public class Box<E extends Floating> {\n"
        "    public Box() { return; }\n"
        "    public E half(float64 acc) { return (E) (acc / 2.0); }\n"
        "}\n"
        "public final class U {\n"
        "    public static float64 run() {\n"
        "        Box<float64> b = heap Box<float64>();\n"
        "        return b.half(7.0);\n"
        "    }\n"
        "}\n", "test.U");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 3.5);
}

// 3 — the primitive path is unchanged (it already worked; pin it so the fix
// cannot regress the case that motivated the grammar order).
TEST(CastTests, primitiveCastOfParenthesizedOperandStillWorks) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int64 mm = 70000L;\n"
        "return (int64) (mm & 65535L);"), "test.U");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 4464);
}

// 4 — PRECEDENCE PIN (spec 3.2). A cast of an UNPARENTHESIZED postfix chain
// must keep binding the whole chain: `(T) a.b()` is `(T) (a.b())`, never
// `((T) a).b()`. A naive grammar reorder breaks exactly this.
TEST(CastTests, castBindsWholePostfixChainNotJustTheReceiver) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public final class U {\n"
        "    static float64 val() { return 6.25; }\n"
        "    public static int32 run() {\n"
        "        return (int32) U.val();\n"
        "    }\n"
        "}\n", "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 6);
}

// 5 — DOCUMENTED DIVERGENCE, and an INCONSISTENCY worth knowing about.
//
// `(T)(x).suffix()` where T names a TYPE parses outermost as DOT over the
// postfix-call node `(T)(x)`, so cajeta reads it cast-first:
// `((T)(x)).suffix()`. Java reads `(T)((x).suffix())`.
//
// The inconsistency: with a PRIMITIVE destination the postfix-call
// alternative cannot match at all (`(int64)` is not an expression), so the
// cast alternative takes the WHOLE chain and cajeta agrees with Java —
// pinned by primitiveCastBindsWholeChainLikeJava below. So the two
// destinations read `(D)(x).f()` differently. The form is rare and
// reader-hostile; the split is accepted and pinned HERE rather than left to
// be discovered. Parenthesize if you mean something else.
TEST(CastTests, typeNameCastOfParenthesizedThenSuffixBindsCastFirst) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public class A {\n"
        "    int32 v;\n"
        "    public A(int32 v) { this.v = v; return; }\n"
        "    public int32 get() { return this.v; }\n"
        "}\n"
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        A a = heap A(41);\n"
        "        return (A) (a).get();\n"
        "    }\n"
        "}\n", "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 41);   // ((A)(a)).get() — cast first, then the suffix
}

// 5b — the other side of that split: a PRIMITIVE destination binds the whole
// postfix chain, matching Java. This already works; pinned so the fix for the
// type-name case cannot quietly change it.
TEST(CastTests, primitiveCastBindsWholeChainLikeJava) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public final class U {\n"
        "    static float64 val() { return 6.75; }\n"
        "    public static int32 run() {\n"
        "        return (int32) (U.val());\n"
        "    }\n"
        "}\n", "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 6);
}
