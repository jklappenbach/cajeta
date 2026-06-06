//
// SharedPoolHarnessTests.cpp — NET-4.3 Model-B (event-driven shared-pool).
//
// Pins the pure-dispatch contract of the NET-4.3 shared-pool server
// (runtime/src/cajeta/net/SharedPoolServer.cajeta), modeled one level down
// via SharedPoolHarness.h (a std::mutex+condvar BoundedQueue standing in for
// cajeta.threading.Channel<TcpStream>, std::atomic for AtomicInt32, and
// std::thread for the worker fibers), in the deterministic, scheduler-free
// posture the other net harnesses use while the cajeta-surface accept loop is
// still being wired.
//
// The defining Model-B property vs. Model A (fiber-per-connection) is that
// the peak number of fibers *running a handler* is bounded by the pool size
// regardless of how many clients connect — the
// `ServerTests.sharedPoolBoundsConcurrentFibers` acceptance the plan names.
// The full in-scheduler version (a live accept loop + worker pool serving 500
// loopback clients) is that JIT ServerTests.* suite; this is its native
// analog and the contract those tests reuse.
//

#include "gtest/gtest.h"

#include "net/SharedPoolHarness.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

using namespace cajeta::net::testing;

namespace {

    // A handler that simply records being run. Sleeps briefly so multiple
    // connections can pile into the queue and the bound is meaningful.
    struct CountingHandler {
        std::atomic<int>* sum;
        std::chrono::microseconds work;
        void operator()(int connId) const {
            std::this_thread::sleep_for(work);
            sum->fetch_add(connId, std::memory_order_seq_cst);
        }
    };

} // namespace

// --- the BoundedQueue mirrors Channel: FIFO, blocking, close-drains --------
// receive() drains buffered items even after close(), then reports
// closed-and-drained (false) — exactly Channel.receive's empty Optional.
TEST(SharedPoolHarnessTests, boundedQueueDrainsThenReportsClosed) {
    BoundedQueue<int> q(4);
    q.send(10);
    q.send(20);
    q.close();                 // close with items still buffered

    int v = -1;
    EXPECT_TRUE(q.receive(v));  // buffered items still drain post-close
    EXPECT_EQ(10, v);
    EXPECT_TRUE(q.receive(v));
    EXPECT_EQ(20, v);
    EXPECT_FALSE(q.receive(v)); // now closed AND drained — worker's exit signal
}

// --- every dispatched connection is handled exactly once (full drain) ------
// N connections through a pool of W workers: each runs exactly once, the
// summed connection ids match, and after a graceful shutdown both the
// in-flight and the queued counters return to zero.
TEST(SharedPoolHarnessTests, allConnectionsHandledExactlyOnce) {
    std::atomic<int> sum{0};
    CountingHandler h{&sum, std::chrono::microseconds(50)};
    SharedPool<CountingHandler> pool(/*poolSize*/4, h);
    pool.start();

    constexpr int conns = 200;
    int expected = 0;
    for (int i = 1; i <= conns; ++i) {
        pool.dispatch(i);
        expected += i;
    }
    pool.shutdown();   // close queue, workers drain remaining, join

    EXPECT_EQ(conns, pool.handledCount()) << "every dispatched connection handled once";
    EXPECT_EQ(expected, sum.load()) << "exactly-once: summed ids match";
    EXPECT_EQ(0, pool.inflight()) << "in-flight drains to zero after a clean stop";
    EXPECT_EQ(0, pool.queued())   << "queued drains to zero after a clean stop";
}

// --- the Model-B bound: peak concurrent handlers stays <= pool size --------
// The headline acceptance (sharedPoolBoundsConcurrentFibers): 500 clients, a
// pool of 8, peak concurrent handler turns stays <= 8 — because the only
// things that run a handler are the 8 workers, no matter how many connect.
TEST(SharedPoolHarnessTests, peakConcurrentHandlersBoundedByPoolSize) {
    std::atomic<int> sum{0};
    // A slightly longer handler so connections genuinely contend for workers.
    CountingHandler h{&sum, std::chrono::microseconds(200)};
    constexpr int32_t poolSize = 8;
    // Capacity larger than the pool so many connections sit queued at once —
    // making it possible (if the bound were broken) to exceed poolSize.
    SharedPool<CountingHandler> pool(poolSize, /*capacity*/64, h);
    pool.start();

    constexpr int conns = 500;
    for (int i = 1; i <= conns; ++i) pool.dispatch(i);
    pool.shutdown();

    EXPECT_EQ(conns, pool.handledCount());
    EXPECT_LE(pool.peakConcurrentHandlers(), poolSize)
        << "peak concurrent handler fibers must stay within the pool size";
    EXPECT_GE(pool.peakConcurrentHandlers(), 1)
        << "the pool actually ran handlers concurrently";
}

// --- a throwing handler turn still drops in-flight; the worker survives ----
// runTurn drops the in-flight count in a `finally`, so a handler that throws
// still decrements and the worker loops on to the next connection — the pool
// is never torn down by one bad turn.
TEST(SharedPoolHarnessTests, throwingHandlerStillDrainsAndPoolSurvives) {
    std::atomic<int> ran{0};
    auto handler = [&ran](int connId) {
        ran.fetch_add(1, std::memory_order_seq_cst);
        if (connId % 3 == 0) throw std::runtime_error("handler blew up");
    };
    SharedPool<std::function<void(int)>> pool(/*poolSize*/3,
                                              std::function<void(int)>(handler));
    pool.start();

    constexpr int conns = 60;   // every 3rd throws (20 throwers)
    for (int i = 1; i <= conns; ++i) pool.dispatch(i);
    pool.shutdown();

    EXPECT_EQ(conns, ran.load()) << "every connection ran, throwers included";
    EXPECT_EQ(conns, pool.handledCount())
        << "a throwing turn still counts handled (the connection was processed)";
    EXPECT_EQ(0, pool.inflight())
        << "a throwing handler still drops its in-flight count (the finally)";
    EXPECT_EQ(0, pool.queued());
}

// --- a single-worker pool serializes: peak concurrency is exactly 1 --------
// With poolSize 1 the work queue is drained by one worker, so no two handler
// turns ever overlap — peak concurrency is 1, and all work still drains.
TEST(SharedPoolHarnessTests, singleWorkerSerializesTurns) {
    std::atomic<int> sum{0};
    CountingHandler h{&sum, std::chrono::microseconds(100)};
    SharedPool<CountingHandler> pool(/*poolSize*/1, /*capacity*/16, h);
    pool.start();

    constexpr int conns = 50;
    for (int i = 1; i <= conns; ++i) pool.dispatch(i);
    pool.shutdown();

    EXPECT_EQ(conns, pool.handledCount());
    EXPECT_EQ(1, pool.peakConcurrentHandlers())
        << "a one-worker pool never overlaps two handler turns";
    EXPECT_EQ(0, pool.inflight());
}

// --- graceful shutdown drains staged work before exiting -------------------
// Connections already staged in the queue at shutdown() time are NOT dropped:
// close() lets the workers drain the buffered items first, then exit. Pin it
// by staging a burst against a slow single worker, then shutting down — all
// staged connections must still be handled (no connection dropped mid-drain).
TEST(SharedPoolHarnessTests, gracefulShutdownDrainsStagedWork) {
    std::atomic<int> sum{0};
    // Slow handler + one worker so the dispatched burst genuinely backs up in
    // the queue, guaranteeing staged-but-unhandled work exists at shutdown.
    CountingHandler h{&sum, std::chrono::milliseconds(2)};
    SharedPool<CountingHandler> pool(/*poolSize*/1, /*capacity*/32, h);
    pool.start();

    constexpr int conns = 24;
    int expected = 0;
    for (int i = 1; i <= conns; ++i) { pool.dispatch(i); expected += i; }

    // Shutdown immediately — most connections are still queued, not yet run.
    pool.shutdown();   // close + drain + join: must finish the staged work

    EXPECT_EQ(conns, pool.handledCount())
        << "graceful shutdown drains staged connections — none dropped";
    EXPECT_EQ(expected, sum.load());
    EXPECT_EQ(0, pool.inflight());
    EXPECT_EQ(0, pool.queued());
}
