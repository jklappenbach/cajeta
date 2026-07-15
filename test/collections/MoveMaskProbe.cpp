// Probe: caller-side `#x` into a plain-`T` param must be observable inside the
// callee via Cajeta.moveMask() (the ownership-transfer signal that lets a
// container take ownership per-call). Borrow call -> bit clear; `#` call -> set.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

TEST(MoveMaskProbe, plainVsTransferredArgVisibleToCallee) {
    auto src =
        "package test;\n"
        "public final class M {\n"
        "    public M() { return; }\n"
        "    public int32 mark(String key) {\n"
        "        return (int32) Cajeta.moveMask();\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        M m = heap M();\n"
        "        String a = \"a\" + \"b\";\n"
        "        int32 borrow = m.mark(a);\n"      // no # -> mask 0
        "        String b = \"c\" + \"d\";\n"
        "        int32 moved = m.mark(#b);\n"      // #  -> mask bit0 = 1
        "        return borrow * 10 + moved;\n"    // expect 0*10 + 1 = 1
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Second arg transferred -> bit1 set, bit0 clear.
TEST(MoveMaskProbe, secondArgTransferredSetsBit1) {
    auto src =
        "package test;\n"
        "public final class M {\n"
        "    public M() { return; }\n"
        "    public int32 mark2(String x, String y) {\n"
        "        return (int32) Cajeta.moveMask();\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        M m = heap M();\n"
        "        String x = \"a\" + \"b\";\n"
        "        String y = \"c\" + \"d\";\n"
        "        return m.mark2(x, #y);\n"   // bit1 only -> 2
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

// title-tracking 7.2.2 — the mask is the callee's OWN trailing ABI
// transfer word, not a TLS: an intervening `#`-bearing call inside the
// body must not clobber what the method entry saw.
TEST(MoveMaskProbe, maskSurvivesInterveningTransferCall) {
    auto src =
        "package test;\n"
        "public final class M {\n"
        "    public M() { return; }\n"
        "    public void eat(#String s) { return; }\n"
        "    public int32 markLate(String key) {\n"
        "        String tmp = \"x\" + \"y\";\n"
        "        this.eat(#tmp);\n"
        "        return (int32) Cajeta.moveMask();\n"
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
// body's Cajeta.moveMask() reads ITS call's bits, not a caller's stale
// state.
TEST(MoveMaskProbe, ctorReadsItsOwnWord) {
    auto src =
        "package test;\n"
        "public final class W {\n"
        "    public int32 got;\n"
        "    public W(String s) {\n"
        "        this.got = (int32) Cajeta.moveMask();\n"
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
