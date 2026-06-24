// Unit 1 (frame-arena-plan) — the thread-local bump arena runtime, tested directly
// via its extern "C" surface. The arena backs non-escaping owned-local allocations:
// bump to allocate, O(1) reset to reclaim, not live-set tracked.

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <thread>

extern "C" {
    void*   __cajeta_arena_alloc(uint64_t size);
    void*   __cajeta_arena_alloc_uninit(uint64_t size);
    uint64_t __cajeta_arena_mark(void);
    void    __cajeta_arena_reset(uint64_t mark);
    int64_t __cajeta_arena_bytes(void);       // current bytes in use
    int64_t __cajeta_arena_retained(void);    // high-water bytes physically retained
    // Live-set introspection (already present) to prove arena blocks aren't tracked.
    int64_t __cajeta_live_set_population(void);
}

// 1.1.1 — distinct, writable, 8-byte-aligned blocks usable as storage.
TEST(ArenaRuntime, allocDistinctAlignedWritable) {
    uint64_t base = __cajeta_arena_mark();
    void* a = __cajeta_arena_alloc(16);
    void* b = __cajeta_arena_alloc(16);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(a) % 8, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(b) % 8, 0u);
    std::memset(a, 0xAB, 16);
    std::memset(b, 0xCD, 16);
    EXPECT_EQ(reinterpret_cast<unsigned char*>(a)[0], 0xAB);
    EXPECT_EQ(reinterpret_cast<unsigned char*>(b)[15], 0xCD);
    __cajeta_arena_reset(base);
}

// 1.1.2 — mark/alloc/reset returns bytes-in-use to the marked level; region reused.
TEST(ArenaRuntime, resetRewindsAndReuses) {
    uint64_t base = __cajeta_arena_mark();
    void* first = __cajeta_arena_alloc(64);
    EXPECT_GE(__cajeta_arena_bytes() - (int64_t) base, 64);
    __cajeta_arena_reset(base);
    EXPECT_EQ(__cajeta_arena_bytes(), (int64_t) base);
    void* again = __cajeta_arena_alloc(64);
    EXPECT_EQ(first, again);   // same bytes handed back out
    __cajeta_arena_reset(base);
}

// 1.1.3 — nested marks reset LIFO; inner reset leaves outer region intact.
TEST(ArenaRuntime, nestedMarksLifo) {
    uint64_t outer = __cajeta_arena_mark();
    void* o = __cajeta_arena_alloc(32);
    std::memset(o, 0x11, 32);
    uint64_t inner = __cajeta_arena_mark();
    __cajeta_arena_alloc(32);
    __cajeta_arena_reset(inner);
    // outer allocation still valid + intact after inner reset.
    EXPECT_EQ(reinterpret_cast<unsigned char*>(o)[0], 0x11);
    EXPECT_EQ(__cajeta_arena_bytes(), (int64_t) inner);
    __cajeta_arena_reset(outer);
    EXPECT_EQ(__cajeta_arena_bytes(), (int64_t) outer);
}

// 1.1.4 — growth: many MB of allocations succeed and stay valid across the span.
TEST(ArenaRuntime, growsBeyondInitialAndStaysValid) {
    uint64_t base = __cajeta_arena_mark();
    const int N = 4096;
    std::vector<unsigned char*> ps;
    for (int i = 0; i < N; ++i) {
        auto* p = reinterpret_cast<unsigned char*>(__cajeta_arena_alloc_uninit(4096));
        p[0] = (unsigned char) (i & 0xff);
        p[4095] = (unsigned char) ((i >> 8) & 0xff);
        ps.push_back(p);
    }
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(ps[i][0], (unsigned char) (i & 0xff));
        EXPECT_EQ(ps[i][4095], (unsigned char) ((i >> 8) & 0xff));
    }
    __cajeta_arena_reset(base);
}

// 1.1.5 — trim on reset: after reclaiming a large span, retained capacity drops and
// the re-handed region reads back zero (physical pages were returned to the OS).
TEST(ArenaRuntime, trimOnResetReturnsMemory) {
    uint64_t base = __cajeta_arena_mark();
    const uint64_t big = 8u << 20;   // 8 MiB, above the trim threshold
    auto* p = reinterpret_cast<unsigned char*>(__cajeta_arena_alloc_uninit(big));
    std::memset(p, 0xFF, big);
    int64_t retainedHigh = __cajeta_arena_retained();
    EXPECT_GE(retainedHigh - (int64_t) base, (int64_t) big);
    __cajeta_arena_reset(base);
    EXPECT_LE(__cajeta_arena_retained() - (int64_t) base, (int64_t)(4u << 20));
    // Re-touch: trimmed pages come back zero-filled.
    auto* q = reinterpret_cast<unsigned char*>(__cajeta_arena_alloc_uninit(big));
    EXPECT_EQ(q[0], 0x00);
    EXPECT_EQ(q[big - 1], 0x00);
    __cajeta_arena_reset(base);
}

// 1.1.6 — thread-local isolation: a worker's arena is independent of main's.
TEST(ArenaRuntime, threadLocalIsolation) {
    uint64_t base = __cajeta_arena_mark();
    __cajeta_arena_alloc(128);
    int64_t mainBytes = __cajeta_arena_bytes();
    int64_t workerBytes = -1;
    std::thread t([&]{
        uint64_t wbase = __cajeta_arena_mark();
        EXPECT_EQ(wbase, 0u);               // fresh per-thread arena
        __cajeta_arena_alloc(4096);
        workerBytes = __cajeta_arena_bytes() - (int64_t) wbase;
        __cajeta_arena_reset(wbase);
    });
    t.join();
    EXPECT_GE(workerBytes, 4096);
    EXPECT_EQ(__cajeta_arena_bytes(), mainBytes);   // main arena untouched by worker
    __cajeta_arena_reset(base);
}

// 1.3.2 — arena blocks are NOT live-set tracked (no double-free hazard).
TEST(ArenaRuntime, allocNotLiveSetTracked) {
    int64_t before = __cajeta_live_set_population();
    uint64_t base = __cajeta_arena_mark();
    for (int i = 0; i < 100; ++i) __cajeta_arena_alloc(64);
    EXPECT_EQ(__cajeta_live_set_population(), before);   // arena bypasses the live-set
    __cajeta_arena_reset(base);
}
