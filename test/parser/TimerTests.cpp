//
// R9.1 — timer wheel + cooperative timeout. Exercises
// __cajeta_task_wait_timeout (surfaced via the Cajeta.taskWaitTimeout
// intrinsic) end-to-end from cajeta source.
//
// The intrinsic takes a raw `pointer` to an int32 done flag plus a
// CLOCK_MONOTONIC absolute deadline (nanoseconds). Returns 1 if the
// flag flipped to non-zero before the deadline, 0 on timeout. These
// tests run on the main thread (no spawn around the call), so the
// non-fiber pthread_cond_timedwait path is what gets exercised; the
// fiber-side path (timer-thread + parked-fiber wake) lands in R9.3
// once `withTimeout` and the surrounding `await spawn` infrastructure
// gives a natural way to invoke it from inside a fiber. The runtime
// code paths are wired symmetrically — the main-thread test here
// validates the deadline + done-flag invariants that both paths
// share.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <chrono>
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Deadline expires first → returns 0. The done flag is allocated and
// never flipped; the 50ms deadline elapses while the main-thread
// cond_timedwait sleeps. Verified at the source level (return value)
// plus a wall-clock lower bound — the call must NOT return early.
TEST(TimerTests, mainThreadTimeoutWhenDoneNeverFlips) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        pointer p = Cajeta.atomicI32New(0);\n"
        "        int64 deadline = Cajeta.currentTimeNanos() + 50000000;\n"
        "        int32 result = Cajeta.taskWaitTimeout(p, deadline);\n"
        "        Cajeta.atomicI32Destroy(p);\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    auto t0 = std::chrono::steady_clock::now();
    int32_t result = runI32(src);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(result, 0);
    // Slack on both sides: must wait at least ~40ms (allow 10ms scheduler
    // jitter under the 50ms target); upper bound is loose to tolerate
    // CI noise.
    EXPECT_GE(elapsed_ms, 40);
    // Loose upper bound: the JIT compile dominates wall time (~2s on dev
    // hardware). The lower bound above is what catches "didn't wait";
    // this just guards against runaway hangs.
    EXPECT_LE(elapsed_ms, 30000);
}

// Done flag pre-set → returns 1 without ever sleeping. The intrinsic's
// fast-path checks the flag before entering cond_timedwait, so the call
// returns immediately even with a far-future deadline.
TEST(TimerTests, mainThreadCompletesImmediatelyWhenDoneAlreadySet) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        pointer p = Cajeta.atomicI32New(1);\n"
        "        int64 deadline = Cajeta.currentTimeNanos() + 60000000000;\n"
        "        int32 result = Cajeta.taskWaitTimeout(p, deadline);\n"
        "        Cajeta.atomicI32Destroy(p);\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    auto t0 = std::chrono::steady_clock::now();
    int32_t result = runI32(src);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(result, 1);
    // Should be near-instantaneous; the JIT compile dominates the wall
    // time. Use a 60s deadline above so even substantial JIT overhead
    // can't accidentally make this look like a successful timeout.
    EXPECT_LE(elapsed_ms, 30000);
}
