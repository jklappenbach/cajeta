//
// FiberCancelTests.cpp — a fiber parked on the TIMER WHEEL must observe a
// cancellation, so a `Tasks.withTimeout` around it can end the wait.
//
// This pins the property that makes `Reactor.pollPark` abandonable. On every
// host without a dedicated fiber-park engine (Windows/macOS), socket readiness
// is not an epoll park — `pollPark` runs a probe-and-back-off loop that parks
// the FIBER through `Cajeta.fiberSleepNanos` (50us doubling to a 2ms cap) and
// retries. The loop itself has no deadline, by design: the untimed
// `awaitReadable` contract is "wait until ready", the same unbounded wait the
// Linux epoll park performs. What bounds it is an ENCLOSING cancellation.
//
// So the whole deadline story on those hosts rests on one thing — that a
// cancellation delivered while the fiber sits in `fiberSleepNanos` is honored
// at the next resume rather than ignored. If it were ignored, every
// `Tasks.withTimeout` over a socket read on Windows would silently never fire,
// and `withTimeoutInt32` would not merely return late: it `await`s the task
// after signalling cancellation, so it would HANG. That failure mode is
// invisible in review and looks exactly like the network being slow.
//
// The assertion is platform-neutral because the mechanism is: the sleep loop
// below is the same `fiberSleepNanos` park `pollPark` performs, driven by the
// same timer wheel, on Linux as anywhere else. A regression here hangs rather
// than fails — the suite's per-test timeout is what reports it, which is still
// a signal, where an unhonored cancellation on a socket produces none.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

TEST(FiberCancelTests, aFiberParkedOnTheTimerWheelHonorsCancellation) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        // The shape Reactor.pollPark runs: park the fiber, wake, retry,
        // forever. The bound is large enough to outlive the deadline by
        // orders of magnitude, so only cancellation can end it.
        "    public static async int32 spin() {\n"
        "        int32 n = 0;\n"
        "        while (n < 1000000) {\n"
        "            Tasks.sleepMillis(2);\n"
        "            n = n + 1;\n"
        "        }\n"
        "        return n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Task<int32> t = spawn spin();\n"
        "        Optional<int32> r =\n"
        "            Tasks.withTimeoutInt32(Duration.ofMillis(200), t);\n"
        // Empty is the contract: the deadline fired, the body was cancelled
        // and drained, and its sentinel was consumed inside withTimeout.
        "        if (r.isPresent()) { return -1; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");

    // -1 would mean the 1000000-iteration sleep loop finished inside 200ms,
    // which it cannot; a hang means the cancellation never reached the parked
    // fiber and `withTimeout`'s own await never returned.
    EXPECT_EQ(fn(), 1);
}
