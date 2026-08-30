//
// pollable-stdin 1.1.8 — is a fiber parked in `awaitReadable` actually
// interruptible?
//
// This is the half of cabra 4.2.1's blocker (b) that was never measured.
// That note said a reader fiber parked in a blocking `read` cannot be
// cancelled, so `{"op":"shutdown"}` would hang at the scope join. The
// pollable-stdin spec argued (b) "dissolves as a consequence" of
// poll-and-park, because the park is itself a yield point. THAT WAS AN
// ARGUMENT, NOT A MEASUREMENT — this file is the measurement.
//
// It is not self-evident. `Tasks.cajeta` says the body "sees the
// cancellation at its next await" and lists yield points as "await,
// channel receive, Lock.acquire, etc."; `awaitReadable` yields via
// `Cajeta.fiberSleepNanos`, and whether THAT observes a cancellation
// signal is precisely the open question.
//
// The discriminator is wall-clock, taken inside the program. A waiter
// parks for 5s on an idle fd and is cancelled at ~300ms:
//   * cancellation reaches the parked fiber  -> withTimeout returns in
//     roughly its own 300ms.
//   * it does not                            -> withTimeout still AWAITS
//     the task to drain (its documented behaviour for bodies that never
//     observe the signal), so it returns in ~5000ms.
//
// Either way the Optional is empty, so the return value alone proves
// nothing — only the elapsed time separates the two.
//
// Pins (plan ids in brackets):
//   [1.1.8] a parked waiter is cancellable, and the join does not hang
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

#if !defined(_WIN32)

#include <unistd.h>

namespace {
// Inside the guard — see FileReaderAwaitReadableTests for why.
struct Pipe {
    int rd = -1, wr = -1;
    bool ok() const { return rd >= 0 && wr >= 0; }
    void closeBoth() { if (rd>=0) ::close(rd); if (wr>=0) ::close(wr); rd=wr=-1; }
};
Pipe makePipe() {
    int fds[2]; Pipe q;
    if (::pipe(fds) != 0) return q;
    q.rd = fds[0]; q.wr = fds[1];
    return q;
}
} // namespace

// [1.1.8] Returns elapsed ms for the cancel+drain, or a negative code.
TEST(FileReaderAwaitCancelTests, aParkedWaiterIsCancellable) {
    Pipe q = makePipe();
    ASSERT_TRUE(q.ok()) << "pipe() failed";
    auto src =
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.time.Clock;\n"
        "import cajeta.time.Duration;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public final class D {\n"
        "    static int32 sharedFd = " + std::to_string(q.rd) + ";\n"
        "    public static int32 waiter() {\n"
        "        FileReader r = heap FileReader(D.sharedFd);\n"
        "        boolean ok = r.awaitReadable(5000);\n"
        "        if (ok) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // The pipe's write end stays OPEN and nothing is written, so the
        // read end is idle rather than at EOF — a waiter genuinely parks.
        "        Duration d = Duration.ofMillis(300);\n"
        "        Task<int32> t = spawn waiter();\n"
        "        int64 t0 = Clock.nanoTime();\n"
        "        Optional<int32> r = Tasks.withTimeoutInt32(d, t);\n"
        "        int64 ms = (Clock.nanoTime() - t0) / 1000000L;\n"
        "        if (r.isPresent()) { return -2; }\n"
        "        return (int32) ms;\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    int32_t rc = jit->lookup<int32_t (*)()>("run")();

    ASSERT_NE(-2, rc) << "the waiter completed — the fd was not idle, so "
                         "this measured nothing";
    EXPECT_LT(rc, 2000)
        << "cancel+drain took " << rc << "ms against a 300ms timeout on a "
           "5000ms wait — the parked fiber never observed the cancellation, "
           "so cabra's shutdown WOULD hang at the join";
    q.closeBoth();
}

// INSTRUMENT VALIDATION for the test above, and a pin on the runtime
// contract it depends on.
//
// The test above concludes "cancellation reached the parked fiber" from a
// SHORT elapsed time. That inference is only sound if `withTimeoutInt32`
// genuinely drains a task that does NOT observe the signal — if it simply
// returned at its own deadline, a short time would prove nothing and the
// test would be vacuous.
//
// So: the same shape with a body that has NO yield point (a spin to a
// deadline). `Tasks.cajeta` documents these as not interruptible —
// "bodies without yield points still run to natural completion (no
// preemption)" — so withTimeout must take the body's full 3s, not its own
// 300ms. If this ever comes back fast, the test above is measuring
// nothing and must be rewritten.
//
// This varies the MECHANISM (yield point present vs absent) rather than
// the implementation under test, which is the only way to control a
// timing claim whose machinery lives in someone else's code.
TEST(FileReaderAwaitCancelTests, withTimeoutDrainsABodyThatCannotYield) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.time.Clock;\n"
        "import cajeta.time.Duration;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public final class D {\n"
        "    public static int32 spin() {\n"
        // No await, no sleep, no channel: nothing the runtime can
        // interrupt at.
        "        int64 until = Clock.nanoTime() + 3000000000L;\n"
        "        int32 acc = 0;\n"
        "        while (Clock.nanoTime() < until) {\n"
        "            acc = acc + 1;\n"
        "        }\n"
        "        return acc;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Duration d = Duration.ofMillis(300);\n"
        "        Task<int32> t = spawn spin();\n"
        "        int64 t0 = Clock.nanoTime();\n"
        "        Optional<int32> r = Tasks.withTimeoutInt32(d, t);\n"
        "        int64 ms = (Clock.nanoTime() - t0) / 1000000L;\n"
        "        if (r.isPresent()) { return -2; }\n"
        "        return (int32) ms;\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    int32_t rc = jit->lookup<int32_t (*)()>("run")();

    ASSERT_NE(-2, rc) << "the spin finished inside the timeout";
    EXPECT_GE(rc, 2000)
        << "withTimeout returned in " << rc << "ms for a 3000ms "
           "un-interruptible body — it does NOT drain, so "
           "aParkedWaiterIsCancellable's short elapsed proves nothing";
}

#endif // !_WIN32
