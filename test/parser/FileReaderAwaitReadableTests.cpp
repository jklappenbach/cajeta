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
// FIXTURES ARE PIPES, NOT EVENTFDS (plan 3.4.1). The first version used
// `Cajeta.eventfdCreate()`, which is LINUX-ONLY, so this suite was
// `#if defined(__linux__)` and macOS ran none of it — the darwin leg was
// green because these tests did not exist there, which is not the same as
// the feature working. A pipe is the portable equivalent: write end left
// OPEN with nothing written == a not-ready fd; a byte written == ready.
//
// Everything POSIX lives INSIDE the platform guard. A helper or include
// at file scope compiles on every target even when every test in the file
// is guarded out — that is how the mingw leg failed to BUILD on `::pipe`
// while its tests were correctly excluded.
//
// Pins (plan ids in brackets):
//   [1.1.1] data pending           -> READY, immediately
//   [1.1.2] data arriving mid-wait -> the wait wakes
//   [1.1.3] nothing pending        -> NOT READY once the timeout elapses,
//                                     and the wait actually took that long
//   [1.1.4] timeoutMs == 0         -> returns without parking (pure probe)
//   [1.1.6] closed / invalid fd    -> an ERROR distinguishable from a timeout
//   [1.1.7] a parked waiter does not hold its carrier
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

#if !defined(_WIN32)

#include <unistd.h>

namespace {

// A pipe. `rd` is not ready until something is written to `wr`, and `wr`
// stays OPEN so the reader sees "idle" rather than EOF.
struct Pipe {
    int rd = -1;
    int wr = -1;
    bool ok() const { return rd >= 0 && wr >= 0; }
    void closeBoth() {
        if (rd >= 0) ::close(rd);
        if (wr >= 0) ::close(wr);
        rd = wr = -1;
    }
};

Pipe makePipe() {
    int fds[2];
    Pipe p;
    if (::pipe(fds) != 0) return p;
    p.rd = fds[0];
    p.wr = fds[1];
    return p;
}

void poke(const Pipe& p) { ssize_t n = ::write(p.wr, "x", 1); (void) n; }

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}

// Wraps a body in the class shell these JIT tests use. The pipe ends are
// available to the body as `D.RD` / `D.WR`, and `D.poke()` writes a byte.
std::string prog(const Pipe& p, const std::string& body) {
    return
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "import cajeta.io.file.FileWriter;\n"
        "import cajeta.time.Clock;\n"
        "public final class D {\n"
        "    static int32 RD = " + std::to_string(p.rd) + ";\n"
        "    static int32 WR = " + std::to_string(p.wr) + ";\n"
        "    static void poke() {\n"
        "        FileWriter w = heap FileWriter(D.WR);\n"
        "        int8[] one = heap int8[1];\n"
        "        one[0] = (int8) 120;\n"
        "        w.write(one, 1);\n"
        "        w.flush();\n"
        "        return;\n"
        "    }\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
}

} // namespace

// [1.1.1] An fd with data already pending is ready at once. Returns 1 for
// ready, 0 for not-ready — so a broken implementation that always reports
// not-ready fails here rather than silently passing a smoke test.
TEST(FileReaderAwaitReadableTests, dataPendingIsReadyImmediately) {
    Pipe p = makePipe();
    ASSERT_TRUE(p.ok()) << "pipe() failed";
    poke(p);
    int32_t rc = runI32(prog(p,
        "        FileReader r = heap FileReader(D.RD);\n"
        "        boolean ready = r.awaitReadable(1000);\n"
        "        if (ready) { return 1; }\n"
        "        return 0;\n"));
    p.closeBoth();
    EXPECT_EQ(1, rc);
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
    Pipe p = makePipe();
    ASSERT_TRUE(p.ok()) << "pipe() failed";
    int32_t rc = runI32(prog(p,
        "        FileReader r = heap FileReader(D.RD);\n"
        "        int64 t0 = Clock.nanoTime();\n"
        "        boolean ready = r.awaitReadable(200);\n"
        "        int64 ms = (Clock.nanoTime() - t0) / 1000000L;\n"
        "        if (ready) { return -2; }\n"
        "        return (int32) ms;\n"));
    p.closeBoth();
    ASSERT_NE(-2, rc) << "an idle pipe must not report readable";
    EXPECT_GE(rc, 200)
        << "returned not-ready after only " << rc
        << "ms for a 200ms timeout — it never waited";
}

// [1.1.4] A zero timeout is a pure probe: no parking, answer now.
// Same instrument rule as 1.1.3 — the clock is inside the program.
TEST(FileReaderAwaitReadableTests, zeroTimeoutProbesWithoutParking) {
    Pipe p = makePipe();
    ASSERT_TRUE(p.ok()) << "pipe() failed";
    int32_t rc = runI32(prog(p,
        "        FileReader r = heap FileReader(D.RD);\n"
        "        int64 t0 = Clock.nanoTime();\n"
        "        boolean idle = r.awaitReadable(0);\n"
        "        int64 ms = (Clock.nanoTime() - t0) / 1000000L;\n"
        "        D.poke();\n"
        "        boolean live = r.awaitReadable(0);\n"
        "        if (idle) { return -2; }\n"
        "        if (!live) { return -3; }\n"
        "        return (int32) ms;\n"));
    p.closeBoth();
    ASSERT_NE(-2, rc) << "probe reported an idle fd readable";
    ASSERT_NE(-3, rc) << "probe missed a written pipe";
    EXPECT_LT(rc, 50)
        << "a zero-timeout probe took " << rc << "ms — it parked";
}

// [1.1.6] A bad fd is an ERROR, and an error is not a timeout. If both
// collapse to `false`, a serve loop spins forever on a dead fd believing
// it is merely idle — so this must not be reachable by returning false.
TEST(FileReaderAwaitReadableTests, invalidFdReportsErrorNotTimeout) {
    Pipe p = makePipe();          // unused by the body; keeps prog()'s shape
    ASSERT_TRUE(p.ok()) << "pipe() failed";
    int32_t rc = runI32(prog(p,
        "        FileReader r = heap FileReader(0 - 1);\n"
        "        boolean threw = false;\n"
        "        try {\n"
        "            r.awaitReadable(0);\n"
        "        } catch (Throwable t) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (threw) { return 1; }\n"
        "        return 0;\n"));
    p.closeBoth();
    EXPECT_EQ(1, rc)
        << "an invalid fd returned normally — indistinguishable from idle";
}

// [1.1.2] Nothing pending at first, then data arrives before the timeout:
// the wait wakes and reports ready. A writer fiber does the poke, so this
// also exercises the interleaving 1.1.7 measures.
TEST(FileReaderAwaitReadableTests, dataArrivingDuringTheWaitWakesIt) {
    Pipe p = makePipe();
    ASSERT_TRUE(p.ok()) << "pipe() failed";
    auto src =
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "import cajeta.io.file.FileWriter;\n"
        "public final class D {\n"
        "    static int32 RD = " + std::to_string(p.rd) + ";\n"
        "    static int32 WR = " + std::to_string(p.wr) + ";\n"
        "    public static int32 writer() {\n"
        "        Cajeta.fiberSleepNanos(100000000L);\n"   // 100ms
        "        FileWriter w = heap FileWriter(D.WR);\n"
        "        int8[] one = heap int8[1];\n"
        "        one[0] = (int8) 120;\n"
        "        w.write(one, 1);\n"
        "        w.flush();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Task<int32> t = spawn writer();\n"
        "        FileReader r = heap FileReader(D.RD);\n"
        "        boolean ready = r.awaitReadable(10000);\n"
        "        int32 ignored = await t;\n"
        "        if (ready) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    int32_t rc = runI32(src);
    p.closeBoth();
    EXPECT_EQ(1, rc)
        << "the wait did not notice data that arrived while it waited";
}

// [1.1.7] THE INVARIANT THIS WHOLE CHANGE EXISTS FOR: a fiber parked in
// awaitReadable does not hold its carrier, so other fibers keep running.
//
// Every other test in this file passes against a carrier-blocking
// implementation — they only ever have ONE fiber in flight.
//
// THREE instrument defects had to be fixed before this test could see
// anything. Every one was caught by running the carrier-blocking control,
// never by reading the test — each version passed while measuring nothing:
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
    setenv("CAJETA_CARRIERS", "1", 1);          // before the pool starts
    Pipe p = makePipe();
    ASSERT_TRUE(p.ok()) << "pipe() failed";
    auto src =
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "import cajeta.io.file.FileWriter;\n"
        "import cajeta.time.Clock;\n"
        "public final class D {\n"
        "    static int32 RD = " + std::to_string(p.rd) + ";\n"
        "    static int32 WR = " + std::to_string(p.wr) + ";\n"
        "    static int32 okCount;\n"
        "    public static int32 waiter() {\n"
        "        FileReader r = heap FileReader(D.RD);\n"
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
        "        FileWriter w = heap FileWriter(D.WR);\n"
        "        int8[] eight = heap int8[8];\n"
        "        int32 k = 0;\n"
        "        while (k < 8) { eight[k] = (int8) 120; k = k + 1; }\n"
        "        w.write(eight, 8);\n"
        "        w.flush();\n"
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
    ASSERT_NE(-3, rc) << "the observer fiber never completed";
    // The timing bound is checked FIRST and is the diagnostic one: under a
    // carrier-blocking implementation the observer cannot be scheduled
    // until a waiter's 3s timeout frees a carrier.
    EXPECT_LT(rc, 1500)
        << "an observer FIBER needed " << rc
        << "ms to finish a 60ms loop while 8 fibers were parked — "
           "the wait held its carrier";
    int32_t ok = jit->lookup<int32_t (*)()>("okCount")();
    EXPECT_GT(ok, 0) << "no waiter ever saw the write";
    p.closeBoth();
}

#endif // !_WIN32
