//
// CP6f-2a tests for the live-fiber registry in the native runtime copy. The
// registry backs the DAP `threads`/fibers view: fibers register at
// __cajeta_task_run and unregister when the carrier frees a DONE fiber. These
// tests drive the container directly with opaque handles (no carrier/JIT
// needed); the per-fiber accessors (id/frame_top/state) and the dbg_id
// assignment are verified end-to-end against a real spawn program in CP6f-2b.
//
#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

extern "C" {
    void  __cajeta_dbg_fiber_register(void* fiber);
    void  __cajeta_dbg_fiber_unregister(void* fiber);
    int   __cajeta_dbg_fiber_count(void);
    void* __cajeta_dbg_fiber_at(int index);
    // CP6f-2d unit 1: atomic snapshot — copies up to `max` handles (spawn
    // order) under one registry-lock hold and returns the TOTAL live count.
    int   __cajeta_dbg_fiber_snapshot(void** out, int max);
    void  __cajeta_dbg_fiber_reg_reset(void);  // test-only
}

TEST(DbgFiberRegistry, RegisterCountAt) {
    __cajeta_dbg_fiber_reg_reset();
    int a, b, c;  // addresses serve as opaque fiber handles
    __cajeta_dbg_fiber_register(&a);
    __cajeta_dbg_fiber_register(&b);
    __cajeta_dbg_fiber_register(&c);

    EXPECT_EQ(__cajeta_dbg_fiber_count(), 3);
    EXPECT_EQ(__cajeta_dbg_fiber_at(0), &a);
    EXPECT_EQ(__cajeta_dbg_fiber_at(1), &b);
    EXPECT_EQ(__cajeta_dbg_fiber_at(2), &c);
    EXPECT_EQ(__cajeta_dbg_fiber_at(3), nullptr);   // out of range
    EXPECT_EQ(__cajeta_dbg_fiber_at(-1), nullptr);

    __cajeta_dbg_fiber_reg_reset();
}

TEST(DbgFiberRegistry, UnregisterPreservesOrder) {
    __cajeta_dbg_fiber_reg_reset();
    int a, b, c;
    __cajeta_dbg_fiber_register(&a);
    __cajeta_dbg_fiber_register(&b);
    __cajeta_dbg_fiber_register(&c);

    __cajeta_dbg_fiber_unregister(&b);   // remove the middle one
    EXPECT_EQ(__cajeta_dbg_fiber_count(), 2);
    EXPECT_EQ(__cajeta_dbg_fiber_at(0), &a);
    EXPECT_EQ(__cajeta_dbg_fiber_at(1), &c);   // order preserved, no swap-hole

    __cajeta_dbg_fiber_unregister(&a);
    __cajeta_dbg_fiber_unregister(&c);
    EXPECT_EQ(__cajeta_dbg_fiber_count(), 0);

    __cajeta_dbg_fiber_reg_reset();
}

TEST(DbgFiberRegistry, NullAndMissingAreNoOps) {
    __cajeta_dbg_fiber_reg_reset();
    int a, other;
    __cajeta_dbg_fiber_register(nullptr);       // no-op
    EXPECT_EQ(__cajeta_dbg_fiber_count(), 0);

    __cajeta_dbg_fiber_register(&a);
    __cajeta_dbg_fiber_unregister(nullptr);     // no-op
    __cajeta_dbg_fiber_unregister(&other);      // not present -> no-op
    EXPECT_EQ(__cajeta_dbg_fiber_count(), 1);
    EXPECT_EQ(__cajeta_dbg_fiber_at(0), &a);

    __cajeta_dbg_fiber_reg_reset();
}

TEST(DbgFiberRegistry, GrowsBeyondInitialCap) {
    __cajeta_dbg_fiber_reg_reset();
    static int handles[100];
    for (int i = 0; i < 100; i++) __cajeta_dbg_fiber_register(&handles[i]);
    EXPECT_EQ(__cajeta_dbg_fiber_count(), 100);
    EXPECT_EQ(__cajeta_dbg_fiber_at(50), &handles[50]);
    EXPECT_EQ(__cajeta_dbg_fiber_at(99), &handles[99]);

    for (int i = 0; i < 100; i++) __cajeta_dbg_fiber_unregister(&handles[i]);
    EXPECT_EQ(__cajeta_dbg_fiber_count(), 0);

    __cajeta_dbg_fiber_reg_reset();
}

// --- CP6f-2d unit 1: atomic snapshot --------------------------------------
// liveFibers() must read the registry as ONE consistent snapshot rather than
// count() then a loop of at(i) with the lock released between (a TOCTOU that
// races concurrent register/unregister on the still-running carriers).

TEST(DbgFiberRegistry, SnapshotCopiesAllInSpawnOrder) {
    __cajeta_dbg_fiber_reg_reset();
    int a, b, c;
    __cajeta_dbg_fiber_register(&a);
    __cajeta_dbg_fiber_register(&b);
    __cajeta_dbg_fiber_register(&c);

    void* buf[8] = {nullptr};
    int n = __cajeta_dbg_fiber_snapshot(buf, 8);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(buf[0], &a);
    EXPECT_EQ(buf[1], &b);
    EXPECT_EQ(buf[2], &c);

    __cajeta_dbg_fiber_reg_reset();
}

TEST(DbgFiberRegistry, SnapshotReturnsFullCountAndClampsCopyToMax) {
    __cajeta_dbg_fiber_reg_reset();
    int a, b, c;
    __cajeta_dbg_fiber_register(&a);
    __cajeta_dbg_fiber_register(&b);
    __cajeta_dbg_fiber_register(&c);

    void* buf[2] = {nullptr, nullptr};
    int n = __cajeta_dbg_fiber_snapshot(buf, 2);
    EXPECT_EQ(n, 3);          // full live count, so the caller can grow + retry
    EXPECT_EQ(buf[0], &a);    // only the first `max` copied
    EXPECT_EQ(buf[1], &b);

    // Count-only query: NULL/0 returns the count without copying.
    EXPECT_EQ(__cajeta_dbg_fiber_snapshot(nullptr, 0), 3);

    __cajeta_dbg_fiber_reg_reset();
}

// Each snapshot must be internally consistent under concurrent mutation: every
// copied handle is a live, known handle (never NULL/garbage/stale), and the
// copied prefix length never exceeds the returned count. The count()+at()
// path can't guarantee this (index shifts between calls); the single-lock
// snapshot can.
TEST(DbgFiberRegistry, SnapshotIsConsistentUnderConcurrentMutation) {
    __cajeta_dbg_fiber_reg_reset();

    // A fixed pool of known handles the mutator registers/unregisters.
    static int pool[64];
    std::set<void*> known;
    for (auto& h : pool) known.insert(&h);

    std::atomic<bool> stop{false};
    std::thread mutator([&] {
        int i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            void* h = &pool[i % 64];
            __cajeta_dbg_fiber_register(h);
            __cajeta_dbg_fiber_unregister(h);
            i++;
        }
    });

    bool ok = true;
    for (int iter = 0; iter < 200000 && ok; ++iter) {
        void* buf[64] = {nullptr};
        int n = __cajeta_dbg_fiber_snapshot(buf, 64);
        int copied = n < 64 ? n : 64;
        for (int k = 0; k < copied; ++k) {
            if (buf[k] == nullptr || known.find(buf[k]) == known.end()) {
                ok = false;  // torn read / stale / out-of-set handle
                break;
            }
        }
    }
    stop.store(true);
    mutator.join();

    EXPECT_TRUE(ok) << "snapshot returned an inconsistent (torn/stale) view";
    __cajeta_dbg_fiber_reg_reset();
}
