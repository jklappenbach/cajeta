//
// XpuPageCacheTests — cajeta-gfx plan §3 (3.d): the generic residency cache
// (cajeta.xpu.PageCache<K,V>).
//
// PageCache is the feedback-driven residency foundation shared by the
// virtual-geometry and virtual-texture streamers (spec §250): a capacity-bounded
// LRU cache of tiles where the consumer reports the tiles it needs (`touch`),
// loads the misses (`admit`), and the cache evicts the least-recently-used tile
// when full — reporting the victim key (`evictedKey`) so the consumer can free
// that tile's GPU page. It reuses the same LRU machinery as
// cajeta.collection.Cache (a HashMap<K, CacheNode<K,V>> for O(1) lookup paired
// with a doubly-linked MRU/LRU list).
//
// v1 constraint inherited from HashMap: K must be a CLASS type (primitives carry
// no hash()), and keys match by identity — so the tests reuse the same key
// objects across operations. GPU-free (pure host JIT, no device).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile a run() body with cajeta.xpu.PageCache imported and an identity-keyed
// `Tag` class available (a bare class hashes/equals by object identity).
int32_t runPC(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.xpu.PageCache;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// 3.d — the eviction order is LRU and a `touch` re-orders it. Capacity 3; admit
// A,B,C; touch A (so the tail is now B); admit D over the cap -> B is the victim.
// A, C, D stay resident; B does not; evictedKey() is B.
TEST(XpuPageCacheTests, lruEvictOrderWithTouch) {
    EXPECT_EQ(runPC(
        "        PageCache<Tag, int32> pc = heap PageCache<Tag, int32>(3);\n"
        "        Tag a = heap Tag();\n"
        "        Tag b = heap Tag();\n"
        "        Tag c = heap Tag();\n"
        "        Tag d = heap Tag();\n"
        "        pc.admit(a, 1);\n"
        "        pc.admit(b, 2);\n"
        "        pc.admit(c, 3);\n"
        "        if (pc.count() != 3) { return -1; }\n"
        "        if (!pc.touch(a)) { return -2; }\n"         // a -> MRU, tail = b
        "        boolean ev = pc.admit(d, 4);\n"
        "        if (!ev) { return -3; }\n"                   // admitting D evicts
        "        if (pc.count() != 3) { return -4; }\n"
        "        Tag victim = pc.evictedKey();\n"
        "        if (victim != b) { return -5; }\n"           // the LRU victim is B
        "        if (!pc.isResident(a)) { return -6; }\n"
        "        if (pc.isResident(b)) { return -7; }\n"
        "        if (!pc.isResident(c)) { return -8; }\n"
        "        if (!pc.isResident(d)) { return -9; }\n"
        "        return 0;\n"), 0);
}

// 3.d — plain LRU with no touches: capacity 2, admit A,B,C -> A (the oldest) is
// evicted; B and C remain.
TEST(XpuPageCacheTests, lruEvictsOldest) {
    EXPECT_EQ(runPC(
        "        PageCache<Tag, int32> pc = heap PageCache<Tag, int32>(2);\n"
        "        Tag a = heap Tag();\n"
        "        Tag b = heap Tag();\n"
        "        Tag c = heap Tag();\n"
        "        pc.admit(a, 1);\n"
        "        pc.admit(b, 2);\n"
        "        boolean ev = pc.admit(c, 3);\n"
        "        if (!ev) { return -1; }\n"
        "        Tag victim = pc.evictedKey();\n"
        "        if (victim != a) { return -2; }\n"
        "        if (pc.isResident(a)) { return -3; }\n"
        "        if (!pc.isResident(b)) { return -4; }\n"
        "        if (!pc.isResident(c)) { return -5; }\n"
        "        return 0;\n"), 0);
}

// 3.d — feedback hit/miss + value retrieval. touch returns true for a resident
// tile, false for a miss; getOrDefault returns the value on a hit and the caller
// fallback on a miss.
TEST(XpuPageCacheTests, touchFeedbackAndGet) {
    EXPECT_EQ(runPC(
        "        PageCache<Tag, int32> pc = heap PageCache<Tag, int32>(4);\n"
        "        Tag a = heap Tag();\n"
        "        Tag b = heap Tag();\n"          // never admitted
        "        pc.admit(a, 99);\n"
        "        if (!pc.touch(a)) { return -1; }\n"          // hit
        "        if (pc.touch(b)) { return -2; }\n"           // miss
        "        if (pc.getOrDefault(a, 0 - 1) != 99) { return -3; }\n"
        "        if (pc.getOrDefault(b, 0 - 7) != 0 - 7) { return -4; }\n"   // miss -> fallback
        "        if (!pc.isResident(a)) { return -5; }\n"
        "        if (pc.isResident(b)) { return -6; }\n"
        "        return 0;\n"), 0);
}

// 3.d — re-admitting a resident key updates its value and does NOT evict (the
// count stays put, no spurious victim).
TEST(XpuPageCacheTests, readmitUpdatesNoEvict) {
    EXPECT_EQ(runPC(
        "        PageCache<Tag, int32> pc = heap PageCache<Tag, int32>(2);\n"
        "        Tag a = heap Tag();\n"
        "        Tag b = heap Tag();\n"
        "        pc.admit(a, 1);\n"
        "        pc.admit(b, 2);\n"
        "        boolean ev = pc.admit(a, 111);\n"            // already resident
        "        if (ev) { return -1; }\n"
        "        if (pc.count() != 2) { return -2; }\n"
        "        if (pc.getOrDefault(a, 0) != 111) { return -3; }\n"
        "        if (pc.getOrDefault(b, 0) != 2) { return -4; }\n"
        "        return 0;\n"), 0);
}
