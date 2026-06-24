// Unit 3 (frame-arena-plan) — generalize arena routing to non-escaping owned
// heap arrays of primitive elements. A `heap T[N]` local that never escapes the
// frame (not returned, stored, #-transferred, or aliased) is bump-allocated from
// the frame arena (no malloc, no live-set, no free_array drop); an escaping one
// stays on the heap. Element drops are a no-op for primitives, so the arena reset
// fully reclaims it. Cajeta.arenaInUse()/Cajeta.liveCount() observe the path.
// (docs/specs/frame-arena-spec.md §3.1, plan 3.1.)

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
std::unique_ptr<CajetaJit> jitOf(const char* body) {
    std::string src =
        std::string("package test;\n") +
        "public final class A {\n" + body + "}\n";
    return CajetaJit::compile(src, "test.A");
}
}

// 3.1.1 — eligible: a primitive array used only locally is arena'd, so arena bytes
// grow while it's live and it never touches the live-set.
TEST(ArenaArrayProbe, eligiblePrimArrayUsesArena) {
    auto jit = jitOf(
        "    public static int64 probe(int32 n) {\n"
        "        int64 abase = Cajeta.arenaInUse();\n"
        "        int64 lbase = Cajeta.liveCount();\n"
        "        int32[] a = heap int32[16];\n"             // eligible: only used locally
        "        a[0] = n;\n"
        "        int64 aused = Cajeta.arenaInUse() - abase;\n"
        "        int64 lused = Cajeta.liveCount() - lbase;\n"
        "        if (a[0] != n) { return -1; }\n"           // local use (keeps a live)
        "        return aused * 1000 + lused;\n"            // aused>0, lused==0
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    int64_t r = fn(7);
    EXPECT_GT(r / 1000, 0) << "eligible primitive array did not use the arena";
    EXPECT_EQ(r % 1000, 0) << "eligible primitive array touched the live-set";
}

// 3.1.1 (escape) — a returned array escapes to the caller and must stay on the heap.
TEST(ArenaArrayProbe, returnedArrayStaysHeap) {
    auto jit = jitOf(
        "    public static int32 sumFirst(int32 n) {\n"
        "        int32[] a = makeArr(n);\n"                 // a borrows; not arena'd by makeArr
        "        return a[0];\n"
        "    }\n"
        "    static int32[] makeArr(int32 n) {\n"
        "        int64 abase = Cajeta.arenaInUse();\n"
        "        int32[] a = heap int32[8];\n"              // escapes via the return below
        "        a[0] = n + 100;\n"
        "        int64 aused = Cajeta.arenaInUse() - abase;\n"
        "        if (aused != 0) { a[0] = -1; }\n"          // record arena misuse in a[0]
        "        return a;\n"
        "    }\n");
    auto fn = jit->lookup<int32_t (*)(int32_t)>("sumFirst");
    EXPECT_EQ(fn(7), 107) << "returned array was wrongly arena-allocated (UAF/misuse)";
}

// 3.1.4 — mixed scope: one eligible array + one escaping array in the same scope.
// Each is routed correctly under a single mark/reset pair; the eligible one is
// arena'd, the escaping one is heap, and the returned value is valid.
TEST(ArenaArrayProbe, mixedScopeRoutedCorrectly) {
    auto jit = jitOf(
        "    static int32[] held;\n"
        "    public static int64 probe(int32 n) {\n"
        "        int64 abase = Cajeta.arenaInUse();\n"
        "        int32[] local = heap int32[8];\n"          // eligible
        "        int32[] escapes = heap int32[8];\n"        // escapes via store below
        "        local[0] = n;\n"
        "        escapes[0] = n + 1;\n"
        "        A.held = escapes;\n"                       // store -> escapes stays heap
        "        int64 aused = Cajeta.arenaInUse() - abase;\n"
        "        if (local[0] != n) { return -1; }\n"
        "        if (A.held[0] != n + 1) { return -2; }\n"
        "        return aused;\n"                           // exactly one array's worth
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    int64_t r = fn(7);
    EXPECT_GT(r, 0) << "eligible array in mixed scope did not use the arena";
    EXPECT_LT(r, 4096) << "escaping array was also arena'd (both took arena path)";
}

// 3.1.x (loop bound) — per-iteration reset keeps a loop of eligible arrays bounded.
TEST(ArenaArrayProbe, loopArrayBounded) {
    auto jit = jitOf(
        "    public static int64 probe(int32 n) {\n"
        "        int64 abase = Cajeta.arenaInUse();\n"
        "        int64 maxSeen = 0;\n"
        "        int32 j = 0;\n"
        "        int64 acc = 0;\n"
        "        while (j < n) {\n"
        "            int32[] a = heap int32[16];\n"
        "            a[0] = j;\n"
        "            int64 cur = Cajeta.arenaInUse() - abase;\n"
        "            if (cur > maxSeen) { maxSeen = cur; }\n"
        "            acc = acc + (int64) a[0];\n"
        "            j = j + 1;\n"
        "        }\n"
        "        int64 after = Cajeta.arenaInUse() - abase;\n"   // 0: fully reset
        "        if (acc < 0) { return -1; }\n"
        "        return maxSeen * 100000 + after;\n"
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    int64_t r = fn(100000);
    int64_t after = r % 100000;
    int64_t maxSeen = r / 100000;
    EXPECT_EQ(after, 0) << "arena not reset after the loop";
    EXPECT_GT(maxSeen, 0) << "loop array never used the arena";
    EXPECT_LT(maxSeen, 4096) << "arena grew with iteration count — per-iteration reset missing";
}
