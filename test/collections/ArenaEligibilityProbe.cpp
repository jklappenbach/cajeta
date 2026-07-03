// Unit 2 (frame-arena-plan) — escape analysis + arena routing for String concat.
// A non-escaping owned concat local is bump-allocated from the frame arena (no
// malloc, no live-set, no drop entry); an escaping one (returned / #-transferred /
// stored) stays on the heap. Cajeta.arenaInUse() / Cajeta.liveCount() observe which
// path a local took. (specs/archive/frame-arena-spec.md §3-5.)

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

// 2.1.1 — eligible: a concat used only as a borrow (method receiver) is arena'd, so
// arena bytes grow while it's live; and it never touches the live-set.
TEST(ArenaEligibilityProbe, eligibleConcatUsesArena) {
    auto jit = jitOf(
        "    public static int64 probe(int32 n) {\n"
        "        int64 abase = Cajeta.arenaInUse();\n"
        "        int64 lbase = Cajeta.liveCount();\n"
        "        String q = \"key\" + n;\n"               // eligible: only borrowed below
        "        int64 aused = Cajeta.arenaInUse() - abase;\n"
        "        int64 lused = Cajeta.liveCount() - lbase;\n"
        "        int64 guard = (int64) q.count();\n"       // borrow use (keeps q live)
        "        if (guard < 0) { return -1; }\n"
        "        return aused * 1000 + lused;\n"           // aused>0, lused==0
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    int64_t r = fn(7);
    EXPECT_GT(r / 1000, 0) << "eligible concat did not use the arena";
    EXPECT_EQ(r % 1000, 0) << "eligible concat touched the live-set";
}

// 2.1.2 — ineligible via #-transfer: the bench's key shape stays on the heap.
TEST(ArenaEligibilityProbe, movedKeyStaysHeap) {
    auto jit = jitOf(
        "    public static int64 probe(int32 n) {\n"
        "        HashMap<String, int32> m = heap HashMap<String, int32>(64);\n"
        "        int64 abase = Cajeta.arenaInUse();\n"
        "        String k = \"key\" + n;\n"                // #-transferred below -> escapes
        "        int64 aused = Cajeta.arenaInUse() - abase;\n"
        "        m.put(#k, n);\n"
        "        return aused;\n"                          // 0: k took the heap path
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    EXPECT_EQ(fn(7), 0) << "#-transferred key was wrongly arena-allocated";
}

// 2.1.3 — ineligible via return: a returned concat escapes to the caller.
TEST(ArenaEligibilityProbe, returnedConcatStaysHeap) {
    auto jit = jitOf(
        "    static String held;\n"
        "    public static int64 probe(int32 n) {\n"
        "        int64 abase = Cajeta.arenaInUse();\n"
        "        String s = \"key\" + n;\n"                // escapes via the alias-return below
        "        int64 aused = Cajeta.arenaInUse() - abase;\n"
        "        D.held = s;\n"                            // store to static field -> escape
        "        return aused;\n"                          // 0
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    EXPECT_EQ(fn(7), 0) << "escaping (stored) concat was wrongly arena-allocated";
}

// 2.1.5 — loop bound: per-iteration reset keeps arena residency ~one iteration, not
// N. The max observed in-scope arena use stays tiny across a long loop.
TEST(ArenaEligibilityProbe, loopArenaBounded) {
    auto jit = jitOf(
        "    public static int64 probe(int32 n) {\n"
        "        int64 abase = Cajeta.arenaInUse();\n"
        "        int64 maxSeen = 0;\n"
        "        int32 j = 0;\n"
        "        while (j < n) {\n"
        "            String q = \"key\" + j;\n"
        "            int64 cur = Cajeta.arenaInUse() - abase;\n"   // base + one q
        "            if (cur > maxSeen) { maxSeen = cur; }\n"
        "            int64 g = (int64) q.count();\n"
        "            if (g < 0) { return -1; }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        int64 after = Cajeta.arenaInUse() - abase;\n"     // 0: fully reset
        "        return maxSeen * 100000 + after;\n"
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    int64_t r = fn(100000);
    int64_t after = r % 100000;
    int64_t maxSeen = r / 100000;
    EXPECT_EQ(after, 0) << "arena not reset after the loop";
    EXPECT_GT(maxSeen, 0) << "loop concat never used the arena";
    EXPECT_LT(maxSeen, 4096) << "arena grew with iteration count — per-iteration reset missing";
}
