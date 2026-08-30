//
// pollable-stdin unit 1 — `FileReader.awaitReadable(timeoutMs)`.
//
// The capability this exposes already existed: the runtime carries a
// portable single-fd readiness probe (`__cajeta_net_reactor_poll_fd`,
// built on select(), accepting any POSIX fd) plus a cooperative
// `Reactor.pollPark`. Both are package-private to `cajeta.io.net.reactor`,
// so nothing outside the net package could wait on an fd. cabra's serve
// loop recorded this as "cajeta has no non-blocking stdin" for three days
// (cabra plan 4.2.1); it was unexposed, not missing.
//
// These arms use an eventfd as the readiness source — the idiom
// IoReactorTests already uses, and the one that needs no scheduler to be
// deterministic. The pipe / EOF / multi-fiber arms (plan 1.1.2, 1.1.5,
// 1.1.7, 1.1.8) need a live carrier and land with the rest of unit 1.
//
// Pins (plan ids in brackets):
//   [1.1.1] data pending           -> READY, immediately
//   [1.1.3] nothing pending        -> NOT READY once the timeout elapses,
//                                     and the wait actually took that long
//   [1.1.4] timeoutMs == 0         -> returns without parking (pure probe)
//   [1.1.6] closed / invalid fd    -> an ERROR distinguishable from a timeout
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"

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

// Wraps a body in the class shell these JIT tests use.
std::string prog(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "import cajeta.time.Clock;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
}

} // namespace

#if defined(__linux__)

// [1.1.1] An fd with data already pending is ready at once. Returns 1 for
// ready, 0 for not-ready — so a broken implementation that always reports
// not-ready fails here rather than silently passing a smoke test.
TEST(FileReaderAwaitReadableTests, dataPendingIsReadyImmediately) {
    auto src = prog(
        "        int32 fd = Cajeta.eventfdCreate();\n"
        "        if (fd < 0) { return -1; }\n"
        "        Cajeta.eventfdSignal(fd);\n"
        "        FileReader r = heap FileReader(fd);\n"
        "        boolean ready = r.awaitReadable(1000);\n"
        "        if (ready) { return 1; }\n"
        "        return 0;\n");
    EXPECT_EQ(1, runI32(src));
}

// [1.1.3] Nothing pending: the wait reports NOT READY rather than throwing.
//
// The elapsed-time check is the load-bearing half — a wait that returns
// not-ready instantly is indistinguishable from a correct timeout on the
// return value alone.
//
// The clock MUST run inside the cajeta program. Timing `runI32` from C++
// measures JIT compilation (~20s here), which clears any millisecond bar
// no matter what the wait does: the first draft of this test asserted
// `elapsed >= 200` around the whole call and passed vacuously.
TEST(FileReaderAwaitReadableTests, idleFdTimesOutAndActuallyWaits) {
    auto src = prog(
        "        int32 fd = Cajeta.eventfdCreate();\n"
        "        if (fd < 0) { return -1; }\n"
        "        FileReader r = heap FileReader(fd);\n"
        "        int64 t0 = Clock.nanoTime();\n"
        "        boolean ready = r.awaitReadable(200);\n"
        "        int64 ms = (Clock.nanoTime() - t0) / 1000000L;\n"
        "        if (ready) { return -2; }\n"
        "        return (int32) ms;\n");

    int32_t rc = runI32(src);
    ASSERT_NE(-2, rc) << "an idle eventfd must not report readable";
    ASSERT_NE(-1, rc) << "eventfd creation failed";
    EXPECT_GE(rc, 200)
        << "returned not-ready after only " << rc
        << "ms for a 200ms timeout — it never waited";
}

// [1.1.4] A zero timeout is a pure probe: no parking, answer now.
// Same instrument rule as 1.1.3 — the clock is inside the program.
TEST(FileReaderAwaitReadableTests, zeroTimeoutProbesWithoutParking) {
    auto src = prog(
        "        int32 fd = Cajeta.eventfdCreate();\n"
        "        if (fd < 0) { return -1; }\n"
        "        FileReader r = heap FileReader(fd);\n"
        "        int64 t0 = Clock.nanoTime();\n"
        "        boolean idle = r.awaitReadable(0);\n"
        "        int64 ms = (Clock.nanoTime() - t0) / 1000000L;\n"
        "        Cajeta.eventfdSignal(fd);\n"
        "        boolean live = r.awaitReadable(0);\n"
        "        if (idle) { return -2; }\n"
        "        if (!live) { return -3; }\n"
        "        return (int32) ms;\n");

    int32_t rc = runI32(src);
    ASSERT_NE(-2, rc) << "probe reported an idle fd readable";
    ASSERT_NE(-3, rc) << "probe missed a signalled fd";
    ASSERT_NE(-1, rc) << "eventfd creation failed";
    EXPECT_LT(rc, 50)
        << "a zero-timeout probe took " << rc << "ms — it parked";
}

// [1.1.6] A bad fd is an ERROR, and an error is not a timeout. If both
// collapse to `false`, a serve loop spins forever on a dead fd believing
// it is merely idle — so this must not be reachable by returning false.
TEST(FileReaderAwaitReadableTests, invalidFdReportsErrorNotTimeout) {
    auto src = prog(
        "        FileReader r = heap FileReader(-1);\n"
        "        boolean threw = false;\n"
        "        try {\n"
        "            r.awaitReadable(0);\n"
        "        } catch (Throwable t) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (threw) { return 1; }\n"
        "        return 0;\n");
    EXPECT_EQ(1, runI32(src))
        << "an invalid fd returned normally — indistinguishable from idle";
}

// [1.1.2] Nothing pending at first, then data arrives before the timeout:
// the wait wakes and reports ready. A signaller fiber does the write, so
// this also exercises the interleaving 1.1.7 measures.
TEST(FileReaderAwaitReadableTests, dataArrivingDuringTheWaitWakesIt) {
    auto src =
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "public final class D {\n"
        "    static int32 sharedFd;\n"
        "    public static int32 signaller() {\n"
        "        Cajeta.fiberSleepNanos(100000000L);\n"   // 100ms
        "        Cajeta.eventfdSignal(D.sharedFd);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 fd = Cajeta.eventfdCreate();\n"
        "        if (fd < 0) { return -1; }\n"
        "        D.sharedFd = fd;\n"
        "        Task<int32> t = spawn signaller();\n"
        "        FileReader r = heap FileReader(fd);\n"
        "        boolean ready = r.awaitReadable(10000);\n"
        "        int32 ignored = await t;\n"
        "        if (ready) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src))
        << "the wait did not notice data that arrived while it waited";
}

// [1.1.7] THE INVARIANT THIS WHOLE CHANGE EXISTS FOR: a fiber parked in
// awaitReadable does not hold its carrier, so other fibers keep running.
//
// Every other test in this file passes against a carrier-blocking
// implementation — they only ever have ONE fiber in flight. This one
// discriminates by TIME: waiters park with a 3s timeout on an idle fd
// while the main fiber runs a ~60ms loop. Cooperative, the loop finishes
// in roughly its own duration; carrier-blocking, the carriers are all
// consumed and the loop cannot run until a timeout expires (3000ms+).
//
// THREE instrument defects had to be fixed before this test could see
// anything. Every one of them was caught by running the carrier-blocking
// control, never by reading the test — each version passed while
// measuring nothing:
//
//   1. Timing from C++ measures JIT compilation (~20s), which clears any
//      millisecond bound. The clock must be INSIDE the program (cf 1.1.3).
//   2. The scheduler is MULTI-CARRIER — min(nproc, 4) — so ONE blocked
//      carrier is invisible; the observer just runs on another. Fixed by
//      saturating with more waiters than the cap.
//   3. THE REAL ONE: `run()` is called straight from the gtest thread via
//      JIT lookup, so the observing code was NOT on a carrier at all.
//      Blocking every carrier cannot stall a loop running on the OS main
//      thread, which is why saturation alone still passed the control.
//      The observer must itself be a spawned fiber.
//
// So the measured span is spawn -> observer-completed, taken inside the
// program: cooperative, the observer starts at once and the span is its
// own ~60ms; carrier-blocking, it cannot start until a waiter's 3s
// timeout frees a carrier.
//
// (`spawn` is eager — measured, not assumed — so the waiters really are
// in flight before the observer is scheduled.)
TEST(FileReaderAwaitReadableTests, aParkedWaiterDoesNotHoldTheCarrier) {
#if defined(_WIN32)
    _putenv_s("CAJETA_CARRIERS", "1");
#else
    setenv("CAJETA_CARRIERS", "1", 1);          // before the pool starts
#endif
    auto src =
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "import cajeta.time.Clock;\n"
        "public final class D {\n"
        "    static int32 sharedFd;\n"
        "    static int32 okCount;\n"
        "    public static int32 waiter() {\n"
        "        FileReader r = heap FileReader(D.sharedFd);\n"
        "        boolean ok = r.awaitReadable(3000);\n"
        "        if (ok) { D.okCount = D.okCount + 1; }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 okCount() { return D.okCount; }\n"
        // The observer is a FIBER, so it competes for carriers with the
        // waiters. Code in `run()` does not — it is the gtest thread.
        "    public static int32 observer() {\n"
        "        int32 i = 0;\n"
        "        while (i < 60) {\n"
        "            Cajeta.fiberSleepNanos(1000000L);\n"  // 1ms, yields
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 fd = Cajeta.eventfdCreate();\n"
        "        if (fd < 0) { return -1; }\n"
        "        D.sharedFd = fd;\n"
        "        D.okCount = 0;\n"
        "        int64 t0 = Clock.nanoTime();\n"
        // Eight waiters: more than CAJETA_DEFAULT_CARRIERS_CAP (4), so a
        // carrier-blocking implementation runs out of carriers even if
        // the env above was read too late to take effect.
        "        Task<int32> a = spawn waiter();\n"
        "        Task<int32> b = spawn waiter();\n"
        "        Task<int32> c = spawn waiter();\n"
        "        Task<int32> d = spawn waiter();\n"
        "        Task<int32> e = spawn waiter();\n"
        "        Task<int32> f = spawn waiter();\n"
        "        Task<int32> g = spawn waiter();\n"
        "        Task<int32> h = spawn waiter();\n"
        "        Task<int32> o = spawn observer();\n"
        "        int32 obs = await o;\n"
        "        int64 ms = (Clock.nanoTime() - t0) / 1000000L;\n"
        "        Cajeta.eventfdSignal(fd);\n"
        "        int32 wa = await a;\n"
        "        int32 wb = await b;\n"
        "        int32 wc = await c;\n"
        "        int32 wd = await d;\n"
        "        int32 we = await e;\n"
        "        int32 wf = await f;\n"
        "        int32 wg = await g;\n"
        "        int32 wh = await h;\n"
        "        if (obs != 1) { return -3; }\n"
        "        return (int32) ms;\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    int32_t rc = jit->lookup<int32_t (*)()>("run")();
    ASSERT_NE(-1, rc) << "eventfd creation failed";
    ASSERT_NE(-3, rc) << "the observer fiber never completed";
    // The timing bound is checked FIRST and is the diagnostic one: under a
    // carrier-blocking implementation the observer cannot be scheduled
    // until a waiter's 3s timeout frees a carrier.
    EXPECT_LT(rc, 1500)
        << "an observer FIBER needed " << rc
        << "ms to finish a 60ms loop while 8 fibers were parked — "
           "the wait held its carrier";
    int32_t ok = jit->lookup<int32_t (*)()>("okCount")();
    EXPECT_EQ(8, ok) << "only " << ok << "/8 waiters saw the signal";
}

#endif // __linux__
