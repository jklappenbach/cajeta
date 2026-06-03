//
// NET-3.2 — Native reactor lifecycle (lazy init / clean shutdown).
//
// The fiber-park / carrier-wake end-to-end lifecycle assertions the plan names
// (`ReactorTests.shutdownDrainsParkedFibers`) are only meaningfully exercised
// against a live fiber scheduler. What is deterministically pinnable at the
// native layer — and what this file pins — is the **platform-independent
// lifecycle machinery** NET-3.2 lands on top of the NET-3.1 engine ABI:
//
//   - lazy, idempotent init that latches a `started` flag             (lazyInitLatches)
//   - a shutdown wake pipe whose read fd is valid + wakeable          (wakePipeWakesAndDrains)
//   - shutdown drains the NET-3.1 live-registration balance to zero   (shutdownDrainsRegistrations)
//   - shutdown is idempotent + resets `started`                       (shutdownIsIdempotent)
//   - shutdown on a never-started reactor is a clean no-op            (shutdownWhenNeverStartedIsNoop)
//   - re-init after shutdown works (JIT-survives-across-tests safety) (reinitAfterShutdown)
//
// These run with no scheduler, the same discipline NetReactorTests /
// NetSocketTests use. The test binary links the C runtime natively and
// cajeta_runtime.c #includes cajeta_net_reactor_lifecycle.c (after
// cajeta_net_reactor.c), so these `__cajeta_net_*` symbols resolve via
// extern "C".
//

#include "gtest/gtest.h"

#include <cstdint>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#else
#  include <sys/select.h>
#  include <sys/time.h>
#endif

namespace {
    constexpr int32_t IO_READ  = 1;
    constexpr int32_t R_READY  = 1;
}

extern "C" {
    // NET-3.1 ABI reused.
    int32_t __cajeta_net_reactor_init(void);
    int32_t __cajeta_net_reactor_register(int32_t fd, int32_t interest, void* h);
    int32_t __cajeta_net_reactor_active_count(void);

    // NET-3.2 lifecycle surface under test.
    int32_t __cajeta_net_reactor_started(void);
    int32_t __cajeta_net_reactor_shutdown(void);
    int32_t __cajeta_net_reactor_wake_fd(void);
    void    __cajeta_net_reactor_wake(void);
    void    __cajeta_net_reactor_wake_drain(void);
}

// --- lazy init latches the started flag -----------------------------------
// Before any init the reactor is not started; after init it is; after shutdown
// it is not again. init is idempotent (every await calls it).
TEST(NetReactorLifecycleTests, lazyInitLatches) {
    // Start from a known-clean baseline (a prior test may have left it up).
    __cajeta_net_reactor_shutdown();
    EXPECT_EQ(0, __cajeta_net_reactor_started());

    EXPECT_EQ(0, __cajeta_net_reactor_init());
    EXPECT_EQ(1, __cajeta_net_reactor_started());
    // Idempotent.
    EXPECT_EQ(0, __cajeta_net_reactor_init());
    EXPECT_EQ(1, __cajeta_net_reactor_started());

    EXPECT_EQ(0, __cajeta_net_reactor_shutdown());
    EXPECT_EQ(0, __cajeta_net_reactor_started());
}

// --- the wake pipe is a valid, wakeable fd --------------------------------
// After init the wake read fd is valid (>= 0). It starts not-ready (nothing
// written), becomes readable after a wake(), and returns to not-ready after a
// drain() — exactly the shutdown-break semantics a portable-path reactor uses.
TEST(NetReactorLifecycleTests, wakePipeWakesAndDrains) {
    __cajeta_net_reactor_shutdown();
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    int32_t wfd = __cajeta_net_reactor_wake_fd();
    ASSERT_GE(wfd, 0) << "wake pipe read fd should be valid after init";

    auto readable = [&](int ms) -> bool {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET((unsigned) wfd, &rfds);
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
#if defined(_WIN32)
        int n = select(0, &rfds, nullptr, nullptr, &tv);
#else
        int n = select(wfd + 1, &rfds, nullptr, nullptr, &tv);
#endif
        return n > 0;
    };

    // Quiescent: not readable.
    EXPECT_FALSE(readable(50)) << "wake pipe should be quiet before a wake";

    // Wake → becomes readable.
    __cajeta_net_reactor_wake();
    EXPECT_TRUE(readable(1000)) << "wake() should make the wake pipe readable";

    // Drain → quiet again.
    __cajeta_net_reactor_wake_drain();
    EXPECT_FALSE(readable(50)) << "drain() should consume the wake byte";

    __cajeta_net_reactor_shutdown();
}

// --- shutdown drains the NET-3.1 live-registration balance -----------------
// A run of register() bumps NET-3.1's active count; a clean shutdown forces it
// back to zero (the "drain registrations" step) — the signal the plan's
// ReactorTests.timedReadDeregistersOnTimeout / shutdownDrainsParkedFibers key
// on, pinned here without a scheduler.
TEST(NetReactorLifecycleTests, shutdownDrainsRegistrations) {
    __cajeta_net_reactor_shutdown();
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    // Simulate three in-flight awaits arming (positive fds, never deregistered
    // — e.g. a crash/cancel path that skipped cleanup).
    __cajeta_net_reactor_register(10, IO_READ, nullptr);
    __cajeta_net_reactor_register(11, IO_READ, nullptr);
    __cajeta_net_reactor_register(12, IO_READ, nullptr);
    EXPECT_EQ(3, __cajeta_net_reactor_active_count());

    EXPECT_EQ(0, __cajeta_net_reactor_shutdown());
    EXPECT_EQ(0, __cajeta_net_reactor_active_count())
        << "shutdown must drain the live-registration balance to zero";
}

// --- shutdown is idempotent + resets started ------------------------------
TEST(NetReactorLifecycleTests, shutdownIsIdempotent) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    EXPECT_EQ(0, __cajeta_net_reactor_shutdown());
    EXPECT_EQ(0, __cajeta_net_reactor_started());
    // Second + third shutdown are clean no-ops.
    EXPECT_EQ(0, __cajeta_net_reactor_shutdown());
    EXPECT_EQ(0, __cajeta_net_reactor_shutdown());
    EXPECT_EQ(0, __cajeta_net_reactor_started());
}

// --- shutdown on a never-started reactor is a no-op -----------------------
// A program that never did network I/O pays nothing at teardown.
TEST(NetReactorLifecycleTests, shutdownWhenNeverStartedIsNoop) {
    // Ensure not started.
    __cajeta_net_reactor_shutdown();
    ASSERT_EQ(0, __cajeta_net_reactor_started());

    EXPECT_EQ(0, __cajeta_net_reactor_shutdown());
    EXPECT_EQ(0, __cajeta_net_reactor_started());
    // No wake pipe exists, so wake/drain are safe no-ops.
    EXPECT_EQ(-1, __cajeta_net_reactor_wake_fd());
    __cajeta_net_reactor_wake();        // no crash
    __cajeta_net_reactor_wake_drain();  // no crash
}

// --- re-init after shutdown works (JIT survives across tests) -------------
// The JIT runtime persists across tests, so a fresh program after a shutdown
// must be able to re-init cleanly with a brand-new wake pipe.
TEST(NetReactorLifecycleTests, reinitAfterShutdown) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t fd1 = __cajeta_net_reactor_wake_fd();
    ASSERT_GE(fd1, 0);
    ASSERT_EQ(0, __cajeta_net_reactor_shutdown());
    EXPECT_EQ(-1, __cajeta_net_reactor_wake_fd())
        << "wake pipe should be closed after shutdown";

    // Re-init stands a fresh reactor back up.
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    EXPECT_EQ(1, __cajeta_net_reactor_started());
    EXPECT_GE(__cajeta_net_reactor_wake_fd(), 0)
        << "re-init must reopen the wake pipe";

    __cajeta_net_reactor_shutdown();
}
