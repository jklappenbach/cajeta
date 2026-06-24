// Verifies the String ownership/drop model behind the hashmap-string benchmark:
// owned (mode 0) Strings — concat results, allocating-method copies — are freed
// when their owner drops; view-mode literals and borrowed aliases are not.
// Cajeta.liveCount() reports the live-object population; we diff it across a unit
// of work. (docs/specification/lang/String.md § Memory model.)

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
std::unique_ptr<CajetaJit> jitOf(const char* body) {
    std::string src =
        std::string("package test;\n") +
        "import cajeta.collection.HashMap;\n" +
        "public final class D {\n" + body + "}\n";
    return CajetaJit::compile(src, "test.D");
}
}

// Loop of owned concat Strings (no map). Each must drop at its loop-body scope.
TEST(OwnershipLeakProbe, loopConcatFreed) {
    auto jit = jitOf(
        "    public static int64 spin(int32 n) {\n"
        "        int64 s = 0; int32 j = 0;\n"
        "        while (j < n) { String q = \"key\" + j; s = s + (int64) q.count(); j = j + 1; }\n"
        "        return s;\n"
        "    }\n"
        "    public static int64 run(int32 n) {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        spin(n);\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("run");
    EXPECT_LT(fn(4000), 50) << "owned concat Strings leaked at loop-scope exit";
}

// A borrowed alias must NOT be dropped (the source still owns it) — no double-free.
TEST(OwnershipLeakProbe, borrowAliasNotDoubleFreed) {
    auto jit = jitOf(
        "    public static int64 run() {\n"
        "        int64 acc = 0; int32 j = 0;\n"
        "        while (j < 1000) {\n"
        "            String a = \"key\" + j;\n"
        "            String b = a;\n"            // borrow alias — b must not drop a's buffer
        "            acc = acc + (int64) b.count();\n"
        "            j = j + 1;\n"
        "        }\n"
        "        return acc;\n"                  // clean exit (no UAF/double-free abort) is the assertion
        "    }\n");
    auto fn = jit->lookup<int64_t (*)()>("run");
    // Byte-length sum of "key0".."key999": 10*4 + 90*5 + 900*6 = 5890. A correct
    // borrow (no double-free crash, no clobbered buffer) returns exactly this.
    EXPECT_EQ(fn(), 5890);
}

// Bench-faithful: build a #-keyed HashMap<String,int32> AND do n lookups (each a
// throwaway borrowed "key"+j), repeated over many iterations. The live-set must
// stay BOUNDED — owned keys reclaimed on map drop, lookup temps on loop exit.
TEST(OwnershipLeakProbe, benchScaleStaysBounded) {
    auto jit = jitOf(
        "    public static int64 iter(int32 n) {\n"
        "        HashMap<String, int32> m = heap HashMap<String, int32>(65536);\n"
        "        int32 i = 0;\n"
        "        while (i < n) { String k = \"key\" + i; m.put(#k, i); i = i + 1; }\n"
        "        int64 h = 0; int32 j = 0;\n"
        "        while (j < n) { String q = \"key\" + j; int32 v = m.get(q); if (v == j) { h = h + 1; } j = j + 1; }\n"
        "        return h;\n"
        "    }\n"
        "    public static int64 run(int32 n, int32 iters) {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0; int64 acc = 0;\n"
        "        while (t < iters) { acc = acc + iter(n); t = t + 1; }\n"
        "        if (acc != (int64) n * (int64) iters) { return (int64) -1; }\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t, int32_t)>("run");
    int64_t delta = fn(4000, 8);
    EXPECT_GE(delta, 0);
    EXPECT_LT(delta, 200) << "live-set grew by " << delta
                          << " over 8 iterations — per-iteration leak remains";
}
