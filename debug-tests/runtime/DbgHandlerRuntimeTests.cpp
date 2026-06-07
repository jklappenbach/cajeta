//
// Tests for the runtime debug-safepoint handler hook + fiber id (CP3, layer
// 2), exercising the native runtime object linked into this binary. Verifies
// the settable handler is invoked with the loc id and current fiber id, that
// a null handler just counts (CP2 behavior), and that the main thread reports
// fiber id 0.
//
#include <gtest/gtest.h>

#include <cstdint>

extern "C" {
    // CP5: the handler gained a frame_top (void*) parameter.
    typedef void (*cajeta_dbg_handler_fn)(int32_t loc_id, int fiber_id,
                                          void* frame_top);
    void __cajeta_dbg_set_safepoint_handler(cajeta_dbg_handler_fn fn);
    void __cajeta_dbg_safepoint(int32_t loc_id);
    int  __cajeta_dbg_current_fiber_id(void);
    long __cajeta_dbg_safepoint_count(void);
    void __cajeta_dbg_reset_safepoint_count(void);
}

namespace {
    int g_calls = 0;
    int g_lastLoc = -1;
    int g_lastFiber = -1;
}

extern "C" void cajeta_test_dbg_handler(int32_t loc_id, int fiber_id,
                                        void* frame_top) {
    (void) frame_top;
    g_calls++;
    g_lastLoc = loc_id;
    g_lastFiber = fiber_id;
}

TEST(DbgHandler, HandlerInvokedWithLocAndFiber) {
    g_calls = 0;
    g_lastLoc = -1;
    g_lastFiber = -1;
    __cajeta_dbg_reset_safepoint_count();
    __cajeta_dbg_set_safepoint_handler(&cajeta_test_dbg_handler);

    __cajeta_dbg_safepoint(123);

    EXPECT_EQ(g_calls, 1);
    EXPECT_EQ(g_lastLoc, 123);
    EXPECT_EQ(g_lastFiber, 0);   // main thread -> fiber id 0
    // The counter still ticks alongside the handler.
    EXPECT_EQ(__cajeta_dbg_safepoint_count(), 1);

    __cajeta_dbg_set_safepoint_handler(nullptr);   // restore global state
}

TEST(DbgHandler, NullHandlerJustCounts) {
    __cajeta_dbg_set_safepoint_handler(nullptr);
    __cajeta_dbg_reset_safepoint_count();
    g_calls = 0;

    __cajeta_dbg_safepoint(1);
    __cajeta_dbg_safepoint(2);

    EXPECT_EQ(__cajeta_dbg_safepoint_count(), 2);
    EXPECT_EQ(g_calls, 0);   // detached handler not called
}

TEST(DbgHandler, CurrentFiberIdZeroOnMainThread) {
    EXPECT_EQ(__cajeta_dbg_current_fiber_id(), 0);
}
