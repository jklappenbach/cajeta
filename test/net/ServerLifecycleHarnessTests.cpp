//
// ServerLifecycleHarnessTests.cpp — NET-4.1 server-core lifecycle + drain.
//
// Pins the pure-logic contract of the NET-4.1 `Server` core
// (runtime/src/cajeta/io/net/Server.cajeta + ServerState.cajeta), modeled one
// level down via ServerLifecycleHarness.h (std::atomic standing in for
// cajeta.concurrent.AtomicInt32), in the deterministic, scheduler-free
// posture the other net harnesses use while the cajeta-surface accept loop
// is still being wired. Two groups:
//
//   * the lifecycle state machine — NEW → RUNNING → DRAINING → STOPPED, with
//     serve() start-once and shutdown() latch idempotency falling straight
//     out of the compare-and-set transitions; and
//   * graceful-drain accounting — in-flight enter/leave (incl. a throwing
//     handler still leaving) and the deadline-bounded drain returning clean
//     vs. forced.
//
// The full in-scheduler version (a live accept loop serving N loopback
// clients, then a graceful shutdown draining them) is the JIT
// `ServerTests.*` suite the plan names (fiberPerConnServes500Clients,
// gracefulShutdownDrainsInflight, ...); this is its native analog and the
// contract those tests reuse.
//

#include "gtest/gtest.h"

#include "net/ServerLifecycleHarness.h"

#include <thread>
#include <vector>

using namespace cajeta::net::testing;
namespace ss = cajeta::net::testing::server_state;

// --- lifecycle: a fresh server starts in NEW ------------------------------
TEST(ServerLifecycleHarnessTests, freshServerStartsNew) {
    ServerLifecycle s;
    EXPECT_EQ(ss::NEW, s.state());
    EXPECT_FALSE(s.isAccepting()) << "a NEW (not-yet-served) server is not accepting";
}

// --- lifecycle: serve() flips NEW → RUNNING exactly once ------------------
// The start-once guard: the first tryStartServing() performs the CAS and
// returns true; a second finds the state already past NEW and returns false
// (the Cajeta `serve()` early-returns on that — an accept loop is never
// started twice on one server).
TEST(ServerLifecycleHarnessTests, serveStartsOnceThenRefuses) {
    ServerLifecycle s;
    EXPECT_TRUE(s.tryStartServing());
    EXPECT_EQ(ss::RUNNING, s.state());
    EXPECT_TRUE(s.isAccepting());

    EXPECT_FALSE(s.tryStartServing()) << "a second serve() must be a no-op";
    EXPECT_EQ(ss::RUNNING, s.state());
}

// --- lifecycle: shutdown latches RUNNING → DRAINING, idempotently ---------
// requestShutdown() fires the RUNNING→DRAINING CAS at most once. The first
// call latches (true) and the loop's isAccepting() goes false; a second call
// no longer fires (false) — shutdown() is idempotent.
TEST(ServerLifecycleHarnessTests, shutdownLatchesDrainOnce) {
    ServerLifecycle s;
    ASSERT_TRUE(s.tryStartServing());

    EXPECT_TRUE(s.requestShutdown());
    EXPECT_EQ(ss::DRAINING, s.state());
    EXPECT_FALSE(s.isAccepting())
        << "once draining, the accept loop's RUNNING test fails → stops accepting";

    EXPECT_FALSE(s.requestShutdown()) << "a second shutdown() must not re-latch";
    EXPECT_EQ(ss::DRAINING, s.state());
}

// --- lifecycle: shutdown on a never-served server is a no-op --------------
// shutdown() before serve() finds the state still NEW, the RUNNING→DRAINING
// CAS does not fire, and the Cajeta core returns true immediately (nothing to
// gracefully stop). The state stays NEW — shutdown did not spuriously advance.
TEST(ServerLifecycleHarnessTests, shutdownBeforeServeIsNoOp) {
    ServerLifecycle s;
    EXPECT_FALSE(s.requestShutdown())
        << "RUNNING→DRAINING cannot fire from NEW — shutdown is a no-op";
    EXPECT_EQ(ss::NEW, s.state());
}

// --- lifecycle: full path settles on terminal STOPPED ---------------------
TEST(ServerLifecycleHarnessTests, fullLifecycleReachesStopped) {
    ServerLifecycle s;
    ASSERT_TRUE(s.tryStartServing());     // NEW → RUNNING
    ASSERT_TRUE(s.requestShutdown());     // RUNNING → DRAINING
    s.finishStop();                       // DRAINING → STOPPED
    EXPECT_EQ(ss::STOPPED, s.state());
    EXPECT_FALSE(s.isAccepting());
    // Terminal: a STOPPED server cannot be (re)started.
    EXPECT_FALSE(s.tryStartServing())
        << "STOPPED is terminal — no NEW→RUNNING transition from it";
    EXPECT_EQ(ss::STOPPED, s.state());
}

// --- lifecycle: exactly one of N concurrent serve() calls wins ------------
// The start-once guard must hold under contention: spin up N threads all
// racing tryStartServing(); exactly one wins the CAS, the rest see RUNNING.
TEST(ServerLifecycleHarnessTests, concurrentServeStartsExactlyOnce) {
    ServerLifecycle s;
    constexpr int N = 16;
    std::atomic<int> winners{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < N; ++i) {
        ts.emplace_back([&] {
            if (s.tryStartServing()) winners.fetch_add(1);
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(1, winners.load())
        << "the NEW→RUNNING CAS must admit exactly one starter";
    EXPECT_EQ(ss::RUNNING, s.state());
}

// --- drain: enter/leave track the in-flight count -------------------------
TEST(ServerLifecycleHarnessTests, drainCounterTracksInflight) {
    DrainCounter d;
    EXPECT_EQ(0, d.count());
    d.enter();
    d.enter();
    EXPECT_EQ(2, d.count());
    d.leave();
    EXPECT_EQ(1, d.count());
    d.leave();
    EXPECT_EQ(0, d.count());
}

// --- drain: a throwing handler still leaves (the `finally` contract) ------
// Server.serveConnection drops the in-flight count in a `finally`, so a
// handler that throws still decrements and never strands the drain. Modeled
// here with a C++ try/throw whose `leave()` runs on the exceptional path.
TEST(ServerLifecycleHarnessTests, throwingHandlerStillLeaves) {
    DrainCounter d;
    d.enter();
    EXPECT_EQ(1, d.count());
    bool threw = false;
    try {
        try {
            throw std::runtime_error("handler blew up");
        } catch (...) {
            d.leave();           // the `finally` decrement
            threw = true;
            throw;               // re-raise: isolated to this connection
        }
    } catch (const std::runtime_error&) {
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(0, d.count())
        << "a throwing handler must still drop its in-flight count";
}

// --- drain: clean drain returns true within the deadline ------------------
// In-flight handlers finish (background threads call leave()) before the
// deadline; drainToZero observes zero and reports a clean drain.
TEST(ServerLifecycleHarnessTests, gracefulDrainCompletesWithinDeadline) {
    DrainCounter d;
    constexpr int conns = 8;
    for (int i = 0; i < conns; ++i) d.enter();
    EXPECT_EQ(conns, d.count());

    std::vector<std::thread> handlers;
    for (int i = 0; i < conns; ++i) {
        handlers.emplace_back([&d] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            d.leave();           // handler finishes well inside the deadline
        });
    }
    // 5s budget — generous vs. the 20ms handlers, so this is a clean drain.
    const bool clean = d.drainToZero(5LL * 1000 * 1000 * 1000);
    for (auto& t : handlers) t.join();

    EXPECT_TRUE(clean) << "all handlers finished before the deadline → clean drain";
    EXPECT_EQ(0, d.count()) << "no connection left in flight after a clean drain";
}

// --- drain: deadline forces the stop with work still in flight ------------
// A connection that outlives the (short) deadline must make drainToZero
// report a forced stop (false), and the in-flight count is still non-zero at
// that point — the "deadline hit, connections remain" path.
TEST(ServerLifecycleHarnessTests, drainDeadlineForcesStopWithInflight) {
    DrainCounter d;
    d.enter();   // one stuck connection that never leaves within the budget

    // 50ms budget; the connection is never decremented inside it.
    const bool clean = d.drainToZero(50LL * 1000 * 1000);
    EXPECT_FALSE(clean) << "the deadline must force the stop while work remains";
    EXPECT_EQ(1, d.count()) << "the stuck connection is still counted in flight";

    d.leave();   // tidy up the model
    EXPECT_EQ(0, d.count());
}

// --- drain: a zero-budget drain is the immediate "stop now" path ----------
// Server.drainInflight with a 0ns deadline returns immediately — true iff
// already idle, false if anything is in flight (no waiting).
TEST(ServerLifecycleHarnessTests, zeroBudgetDrainIsImmediate) {
    DrainCounter idle;
    EXPECT_TRUE(idle.drainToZero(0)) << "0-budget drain of an idle server is a clean, instant stop";

    DrainCounter busy;
    busy.enter();
    EXPECT_FALSE(busy.drainToZero(0)) << "0-budget drain with work in flight forces the stop";
    busy.leave();
}
