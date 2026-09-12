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
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Run `src` and report how long the CALL took, with the JIT compile excluded.
// These tests assert on wall-clock, and the compile is both far larger than
// what they measure (seconds vs ~50ms) and load-dependent — under a 32-shard
// sweep it has reached 30s on its own. Timing the whole of runI32 therefore
// measured machine load, which both blew the upper bounds on a busy box and
// made "returned immediately" indistinguishable from "waited". Compile first,
// then start the clock.
int32_t runI32Timed(const std::string& src, int64_t& elapsedMs) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    auto t0 = std::chrono::steady_clock::now();
    int32_t result = fn();
    elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    return result;
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
    int64_t elapsed_ms = 0;
    int32_t result = runI32Timed(src, elapsed_ms);
    EXPECT_EQ(result, 0);
    // Slack on both sides: must wait at least ~40ms (allow 10ms scheduler
    // jitter under the 50ms target). This is what catches "didn't wait".
    EXPECT_GE(elapsed_ms, 40);
    // Upper bound: with the compile excluded this measures the wait itself,
    // so it can be generous and still mean something — a 50ms deadline that
    // overruns by 100x is broken, not busy.
    EXPECT_LE(elapsed_ms, 5000);
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
    int64_t elapsed_ms = 0;
    int32_t result = runI32Timed(src, elapsed_ms);
    EXPECT_EQ(result, 1);
    // The fast path must not sleep. With the compile excluded this is a real
    // assertion: the deadline above is 60s, so a bound well under it fails
    // if the flag check is skipped and the call actually waits. (Timing the
    // compile too made this vacuous — it passed either way.)
    EXPECT_LE(elapsed_ms, 5000);
}

// The timer thread's own wait. `deadline_ns` is CLOCK_MONOTONIC, but
// pthread_cond_timedwait reads abstime on CLOCK_REALTIME, so feeding it the
// raw monotonic value named a moment in 1970 and the wait returned ETIMEDOUT
// instantly — the loop spun for as long as any fiber timer was pending,
// reacquiring the mutex that every fiber park and unpark also needs.
//
// Measured as CPU burned, not as latency or as a runtime counter, and both
// of those alternatives were tried first:
//
//   - Latency cannot see this at all. The sleeping fiber still wakes on
//     schedule; the spin costs a core, not a deadline. Both arms below took
//     305ms of wall clock.
//   - A counter incremented inside the timer loop reads 0 here. The JIT links
//     the EMBEDDED bitcode runtime into the module under test, so it bumps its
//     own copy of the variable while the accessor the test calls resolves to
//     the test binary's separate native copy. That is a blind instrument, and
//     a blind instrument reports health (CLAUDE.md §5).
//
// getrusage(RUSAGE_SELF) escapes both problems: it sums every thread in the
// process regardless of which runtime copy they run, and it is immune to a
// busy box, since a parallel sweep's other suites are separate processes.
//
// A/B over the one-line clock change, same source, same machine:
//     broken   wall 304ms   cpu 217ms
//     fixed    wall 305ms   cpu 6.8ms
#if defined(_WIN32)
TEST(TimerTests, DISABLED_timerThreadSleepsBetweenExpiriesRatherThanSpinning) {}
#else
TEST(TimerTests, timerThreadSleepsBetweenExpiriesRatherThanSpinning) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 napper() {\n"
        "        Tasks.sleepMillis(300);\n"
        "        return 7;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Task<int32> t = spawn napper();\n"
        "        return await t;\n"
        "    }\n"
        "}\n";
    // Compile first: the JIT is seconds of real CPU and would swamp the
    // milliseconds this measures.
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    auto w0 = std::chrono::steady_clock::now();
    int32_t result = fn();
    int64_t wallUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - w0).count();
    getrusage(RUSAGE_SELF, &ru1);

    auto deltaUs = [](const struct timeval& a, const struct timeval& b) {
        return (int64_t) (b.tv_sec - a.tv_sec) * 1000000 + (b.tv_usec - a.tv_usec);
    };
    int64_t cpuUs = deltaUs(ru0.ru_utime, ru1.ru_utime)
                  + deltaUs(ru0.ru_stime, ru1.ru_stime);

    EXPECT_EQ(result, 7);
    // The sleep really has to happen, or there is no pending timer to spin on
    // and the CPU bound below passes vacuously.
    EXPECT_GE(wallUs, 250000) << "the fiber did not actually sleep";
    // A quarter of the window sits ~11x above the healthy 6.8ms and ~3x below
    // the 217ms the spin cost, so neither arm is near the line.
    EXPECT_LT(cpuUs, wallUs / 4)
        << "burned " << cpuUs << "us of CPU across a " << wallUs
        << "us sleep — the timer thread is spinning, not waiting "
           "(CLOCK_MONOTONIC deadline handed to a CLOCK_REALTIME abstime?)";
}
#endif
