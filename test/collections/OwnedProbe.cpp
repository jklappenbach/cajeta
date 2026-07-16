// Probe: caller-side `#x` into a plain-`T` param must be observable inside
// the callee via Cajeta.owned(formal) (title-stores §4 — the per-formal
// successor of the retired positional Cajeta.moveMask()). Borrow call →
// false; `#` call → true. Formerly MoveMaskProbe (title-stores 5.2.2).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

TEST(OwnedProbe, plainVsTransferredArgVisibleToCallee) {
    auto src =
        "package test;\n"
        "public final class M {\n"
        "    public M() { return; }\n"
        "    public int32 mark(String key) {\n"
        "        if (Cajeta.owned(key)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        M m = heap M();\n"
        "        String a = \"a\" + \"b\";\n"
        "        int32 borrow = m.mark(a);\n"      // no # -> false
        "        String b = \"c\" + \"d\";\n"
        "        int32 moved = m.mark(#b);\n"      // #  -> true
        "        return borrow * 10 + moved;\n"    // expect 0*10 + 1 = 1
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Per-formal resolution: second arg transferred → owned(y) true, owned(x)
// false — no positional arithmetic in user code.
TEST(OwnedProbe, secondArgTransferredResolvesPerFormal) {
    auto src =
        "package test;\n"
        "public final class M {\n"
        "    public M() { return; }\n"
        "    public int32 mark2(String x, String y) {\n"
        "        int32 t = 0;\n"
        "        if (Cajeta.owned(x)) { t = t + 1; }\n"
        "        if (Cajeta.owned(y)) { t = t + 2; }\n"
        "        return t;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        M m = heap M();\n"
        "        String x = \"a\" + \"b\";\n"
        "        String y = \"c\" + \"d\";\n"
        "        return m.mark2(x, #y);\n"   // y only -> 2
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

// title-tracking 7.2.2 — the answer is the callee's OWN trailing ABI
// transfer word, not a TLS: an intervening `#`-bearing call inside the
// body must not clobber what the method entry saw.
TEST(OwnedProbe, answerSurvivesInterveningTransferCall) {
    auto src =
        "package test;\n"
        "public final class M {\n"
        "    public M() { return; }\n"
        "    public void eat(#String s) { return; }\n"
        "    public int32 markLate(String key) {\n"
        "        String tmp = \"x\" + \"y\";\n"
        "        this.eat(#tmp);\n"
        "        if (Cajeta.owned(key)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        M m = heap M();\n"
        "        String b = \"c\" + \"d\";\n"
        "        return m.markLate(#b);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Constructors ride the same word (their only per-call channel) — a ctor
// body's Cajeta.owned() reads ITS call's bit, not a caller's stale state.
TEST(OwnedProbe, ctorReadsItsOwnWord) {
    auto src =
        "package test;\n"
        "public final class W {\n"
        "    public int32 got;\n"
        "    public W(String s) {\n"
        "        if (Cajeta.owned(s)) { this.got = 1; } else { this.got = 0; }\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String b = \"c\" + \"d\";\n"
        "        W w = heap W(#b);\n"
        "        return w.got;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
