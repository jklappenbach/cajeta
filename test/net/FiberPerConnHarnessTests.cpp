//
// FiberPerConnHarnessTests.cpp — NET-4.2 fiber-per-connection accept model.
//
// Pins the pure-logic contract of the NET-4.2 dispatch model — the
// Server.dispatch / Server.serveConnection seam in
// runtime/src/cajeta/io/net/Server.cajeta, running inside Server.serve's
// owning scope — modeled one level down via FiberPerConnHarness.h
// (std::thread standing in for a connection fiber, composing the NET-4.1
// DrainCounter / ServerLifecycle verbatim) in the deterministic,
// scheduler-free posture the other net harnesses use while the
// cajeta-surface accept loop is still being wired.
//
// Four groups, one per NET-4.2 contract point:
//
//   * 1:1 accept→spawn          — dispatching n connections spawns exactly
//                                 n fibers (Model A's defining mapping).
//   * structured-concurrency    — the scope-join drains every fiber; after
//     drain                       it, in-flight is zero (the property a
//                                 graceful shutdown awaits).
//   * true concurrency          — K parked handlers reach an in-flight peak
//                                 of K *concurrently* (native analog of the
//                                 JIT ServerTests.fiberPerConnServes500Clients).
//   * handler isolation         — a throwing handler still leaves + does not
//                                 tear down its siblings (the `finally` +
//                                 isolation contract).
//
// The full in-scheduler version (a live accept loop serving 500 loopback
// clients one fiber each, then a graceful shutdown) is the JIT
// `ServerTests.*` suite the plan names; this is its native analog and the
// contract those tests reuse.
//

#include "gtest/gtest.h"

#include "net/FiberPerConnHarness.h"
#include "net/ServerLifecycleHarness.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace cajeta::net::testing;
namespace ss = cajeta::net::testing::server_state;

// A handler-side latch so a test can hold connection fibers "parked"
// (mid-handler) and release them on demand — the deterministic stand-in for
// fibers blocked on a slow client / async read.
namespace {
    struct Gate {
        std::atomic<bool> open{false};
        // Park-style wait that *yields* the carrier (rather than busy-sleeping
        // it), so every connection fiber gets scheduled and becomes live
        // concurrently — the cooperative-yield posture a parked Cajeta fiber
        // takes (it releases its carrier), which a sleep would not model.
        void wait() const {
            while (!open.load(std::memory_order_seq_cst)) {
                std::this_thread::yield();
            }
        }
        void release() { open.store(true, std::memory_order_seq_cst); }
    };
}

// --- 1:1 accept→spawn: n dispatches ⇒ exactly n connection fibers ---------
// Model A's defining mapping: the accept loop spawns one fiber per accepted
// socket. Dispatch n connections and exactly n fibers are spawned — no
// pooling, no coalescing (that is Model B, NET-4.3).
TEST(FiberPerConnHarnessTests, oneFiberPerAcceptedConnection) {
    FiberPerConnDispatcher d;
    constexpr int n = 64;
    std::atomic<int> ran{0};
    for (int i = 0; i < n; ++i) {
        d.dispatch([&ran] { ran.fetch_add(1, std::memory_order_seq_cst); });
    }
    d.joinScope();

    EXPECT_EQ(n, d.spawned()) << "fiber-per-connection: one fiber per accept, no more/fewer";
    EXPECT_EQ(n, ran.load()) << "every spawned fiber ran its handler exactly once";
}

// --- structured concurrency: the scope-join drains in-flight to zero ------
// Every spawn is issued inside Server.serve's scope; the scope's closing
// brace joins all connection fibers, so after the join in-flight is zero
// (every serveConnection `finally` ran). This is the property the graceful
// drain rides on — the join is the real owner, the drain just observes it.
TEST(FiberPerConnHarnessTests, scopeJoinDrainsInflightToZero) {
    FiberPerConnDispatcher d;
    constexpr int n = 32;
    Gate gate;   // hold every handler parked until released
    for (int i = 0; i < n; ++i) {
        d.dispatch([&gate] { gate.wait(); });
    }
    // All n fibers are live and parked: in-flight is n, none has drained.
    // (Spin briefly until every fiber has at least entered.)
    for (int spins = 0; d.inflight().count() < n && spins < 2000000; ++spins) {
        std::this_thread::yield();
    }
    EXPECT_EQ(n, d.inflight().count()) << "all connection fibers in flight while parked";

    gate.release();      // let every handler finish
    d.joinScope();       // the scope's `}` joins them all

    EXPECT_EQ(0, d.inflight().count())
        << "after the scope-join every `finally` ran → in-flight back to zero";
}

// --- structured concurrency: shutdown's drain composes with the join ------
// The graceful-shutdown drain (NET-4.1 DrainCounter::drainToZero) observes
// the same in-flight the scope owns: with the lifecycle latched to DRAINING
// and the handlers released, the drain reaches zero cleanly within budget,
// then the lifecycle settles on STOPPED — no connection dropped mid-flight.
TEST(FiberPerConnHarnessTests, gracefulShutdownDrainsFiberPerConn) {
    FiberPerConnDispatcher d;
    ServerLifecycle life;
    ASSERT_TRUE(life.tryStartServing());          // NEW → RUNNING

    constexpr int n = 16;
    Gate gate;
    for (int i = 0; i < n; ++i) {
        d.dispatch([&gate] { gate.wait(); });
    }

    ASSERT_TRUE(life.requestShutdown());          // RUNNING → DRAINING (stop accepting)
    EXPECT_FALSE(life.isAccepting());

    // Release in-flight handlers shortly after the drain starts, on another
    // thread — they must all finish well inside the 5s budget.
    std::thread releaser([&gate] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        gate.release();
    });

    const bool clean = d.inflight().drainToZero(5LL * 1000 * 1000 * 1000);
    releaser.join();
    d.joinScope();
    life.finishStop();                            // DRAINING → STOPPED

    EXPECT_TRUE(clean) << "all fiber-per-conn handlers drained before the deadline";
    EXPECT_EQ(0, d.inflight().count()) << "no connection left in flight after a clean drain";
    EXPECT_EQ(ss::STOPPED, life.state());
}

// --- true concurrency: K parked handlers reach an in-flight peak of K -----
// The native analog of ServerTests.fiberPerConnServes500Clients: the model
// runs handlers CONCURRENTLY (one live fiber each), so K handlers that all
// park at once drive the in-flight high-water mark to K — not 1, which a
// serialized one-at-a-time dispatch would cap it at. This is the property
// that distinguishes fiber-per-connection (Model A) from a single worker.
TEST(FiberPerConnHarnessTests, parkedHandlersRunConcurrently) {
    FiberPerConnDispatcher d;
    constexpr int K = 200;            // 200 concurrent connection fibers
    Gate gate;
    std::atomic<int> entered{0};
    for (int i = 0; i < K; ++i) {
        d.dispatch([&gate, &entered] {
            entered.fetch_add(1, std::memory_order_seq_cst);
            gate.wait();              // park: hold the fiber in-flight
        });
    }
    // Wait until all K handlers are simultaneously parked in-flight.
    for (int spins = 0; entered.load() < K && spins < 2000000; ++spins) {
        std::this_thread::yield();
    }
    EXPECT_EQ(K, entered.load()) << "all K handlers entered concurrently";
    EXPECT_EQ(K, d.inflight().count())
        << "all K connection fibers are in flight at once (true concurrency)";

    gate.release();
    d.joinScope();

    EXPECT_EQ(K, d.peakInflight())
        << "the in-flight high-water mark reached K — handlers ran concurrently, not serialized";
    EXPECT_EQ(0, d.inflight().count());
}

// --- handler isolation: a throwing handler still leaves + spares siblings -
// serveConnection drops in-flight in a `finally`, so a handler that throws
// still decrements; and the throw is isolated to its own fiber — its
// siblings keep running and complete, and the accept loop is untouched. The
// native analog of the "a handler that throws does not tear down the server"
// contract Server.cajeta documents.
TEST(FiberPerConnHarnessTests, throwingHandlerIsolatedSiblingsSurvive) {
    FiberPerConnDispatcher d;
    constexpr int n = 24;
    std::atomic<int> okCompleted{0};

    for (int i = 0; i < n; ++i) {
        const bool poison = (i % 4 == 1);     // a quarter of handlers throw
        if (poison) {
            d.dispatch([] { throw std::runtime_error("connection handler blew up"); });
        } else {
            d.dispatch([&okCompleted] { okCompleted.fetch_add(1, std::memory_order_seq_cst); });
        }
    }
    d.joinScope();

    const int expectedPoison = (n + 2) / 4;   // i % 4 == 1 over [0,n)
    const int expectedOk = n - expectedPoison;

    EXPECT_EQ(expectedOk, okCompleted.load())
        << "every non-throwing sibling completed — a throw does not tear down the loop";
    EXPECT_EQ(0, d.inflight().count())
        << "throwing handlers still dropped their in-flight count (the `finally`)";

    // And the throws were caught + isolated (recorded on their fibers), never
    // propagated out of the dispatcher.
    EXPECT_EQ(n, d.spawned()) << "all connections were dispatched, throwing ones included";
}

// --- handler isolation under contention: throws never strand the drain ----
// A graceful shutdown drains cleanly even when a fraction of handlers throw:
// the `finally`-decrement on the exceptional path means the in-flight count
// still reaches zero, so the drain reports clean and the lifecycle reaches
// STOPPED — a throwing connection never strands shutdown.
TEST(FiberPerConnHarnessTests, throwingHandlersDoNotStrandDrain) {
    FiberPerConnDispatcher d;
    ServerLifecycle life;
    ASSERT_TRUE(life.tryStartServing());
    ASSERT_TRUE(life.requestShutdown());          // draining

    constexpr int n = 40;
    for (int i = 0; i < n; ++i) {
        if (i % 3 == 0) {
            d.dispatch([] { throw std::runtime_error("boom"); });
        } else {
            d.dispatch([] { /* quick, clean handler */ });
        }
    }

    const bool clean = d.inflight().drainToZero(5LL * 1000 * 1000 * 1000);
    d.joinScope();
    life.finishStop();

    EXPECT_TRUE(clean) << "throwing handlers still decrement → drain reaches zero";
    EXPECT_EQ(0, d.inflight().count());
    EXPECT_EQ(ss::STOPPED, life.state());
}
