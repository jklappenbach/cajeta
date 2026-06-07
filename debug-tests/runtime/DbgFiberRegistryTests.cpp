//
// CP6f-2a tests for the live-fiber registry in the native runtime copy. The
// registry backs the DAP `threads`/fibers view: fibers register at
// __cajeta_task_run and unregister when the carrier frees a DONE fiber. These
// tests drive the container directly with opaque handles (no carrier/JIT
// needed); the per-fiber accessors (id/frame_top/state) and the dbg_id
// assignment are verified end-to-end against a real spawn program in CP6f-2b.
//
#include <gtest/gtest.h>

extern "C" {
    void  __cajeta_dbg_fiber_register(void* fiber);
    void  __cajeta_dbg_fiber_unregister(void* fiber);
    int   __cajeta_dbg_fiber_count(void);
    void* __cajeta_dbg_fiber_at(int index);
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
