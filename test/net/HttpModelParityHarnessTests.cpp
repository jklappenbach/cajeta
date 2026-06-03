//
// HttpModelParityHarnessTests.cpp — NET-9.2 "both accept models serve the
// same handler."
//
// NET-9.2 is the deliverable that the NET-9.1 HttpServer runs UNCHANGED on
// either NET-4 accept model — fiber-per-connection (Model A, NET-4.2) and the
// event-driven shared-pool (Model B, NET-4.3) — selected via the builder. In
// the HttpServer that is structural: the per-connection protocol loop
// (HttpServer.serveConnection) is one model-agnostic (TcpStream)->void
// callback, and ServerModel.bindServer materializes whichever accept stack
// underneath it (Server.bind vs SharedPoolServer.bind) without touching the
// callback. The accept stack only changes HOW that callback is dispatched
// (one fiber per connection vs a bounded worker pool), never WHAT it computes.
//
// This harness pins that property one level down, in the same deterministic,
// scheduler-free posture as FiberPerConnHarness.h (Model A) and
// SharedPoolHarness.h (Model B): drive the SAME handler closure through BOTH
// dispatchers over the same set of "connections" and assert the per-connection
// results are identical (independent of dispatch model). It is the native
// analog of the JIT `HttpServerTests.bothModelsServeSameHandler` acceptance,
// which needs the in-scheduler reactor + loopback sockets the NET-4
// `ServerTests.*` suite awaits.
//
// The handler here stands in for the HttpServer per-connection callback: it
// maps a connection id to a deterministic "response" value (the way the real
// handler maps a parsed request to a response), recording the result keyed by
// connection so the two models' output maps can be compared for equality.
//

#include "gtest/gtest.h"

#include "net/FiberPerConnHarness.h"   // FiberPerConnDispatcher (Model A)
#include "net/SharedPoolHarness.h"     // SharedPool (Model B)

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

using namespace cajeta::net::testing;

namespace {

    // A deterministic stand-in for the HttpServer per-connection handler: a
    // pure function of the connection id (as the real handler is a function of
    // the parsed request). Records its result keyed by connection so the two
    // models' result maps can be compared. Thread-safe (both dispatchers run
    // it from multiple worker threads/fibers concurrently).
    struct RecordingHandler {
        std::map<int, int64_t>* results;
        std::mutex*             mu;

        // The "response" for a connection — any deterministic transform; the
        // point is that it is identical regardless of which model invokes it.
        static int64_t respond(int connId) {
            return static_cast<int64_t>(connId) * 1315423911LL + 2654435761LL;
        }

        void operator()(int connId) const {
            int64_t r = respond(connId);
            std::lock_guard<std::mutex> lk(*mu);
            (*results)[connId] = r;
        }
    };

    // Drive `n` connections through Model A (fiber-per-connection) and return
    // the per-connection result map.
    std::map<int, int64_t> runModelA(int n) {
        std::map<int, int64_t> results;
        std::mutex mu;
        RecordingHandler handler{&results, &mu};

        FiberPerConnDispatcher disp;
        for (int i = 0; i < n; ++i) {
            disp.dispatch([handler, i] { handler(i); });
        }
        disp.joinScope();   // the serve-scope join: every connection fiber done
        return results;
    }

    // Drive `n` connections through Model B (shared-pool, `pool` workers) and
    // return the per-connection result map.
    std::map<int, int64_t> runModelB(int n, int pool) {
        std::map<int, int64_t> results;
        std::mutex mu;
        RecordingHandler handler{&results, &mu};

        SharedPool<RecordingHandler> sp(pool, handler);
        sp.start();
        for (int i = 0; i < n; ++i) {
            sp.dispatch(i);
        }
        sp.shutdown();      // close queue, drain, join workers
        return results;
    }

} // namespace

// --- the same handler yields identical results on both models -----------

// The defining NET-9.2 property: dispatching the same handler over the same
// connections on Model A and Model B yields the SAME per-connection results.
TEST(HttpModelParityHarnessTests, sameHandlerSameResultsAcrossModels) {
    const int n = 500;        // the acceptance-shaped fan-in (500 clients)
    const int pool = 8;       // a bounded pool of 8 (cf. the NET-4.3 acceptance)

    std::map<int, int64_t> a = runModelA(n);
    std::map<int, int64_t> b = runModelB(n, pool);

    // Every connection was handled exactly once on each model (full drain, no
    // drops, no double-handling).
    ASSERT_EQ(static_cast<int>(a.size()), n);
    ASSERT_EQ(static_cast<int>(b.size()), n);

    // And the per-connection results are byte-for-byte identical — the handler
    // is model-independent.
    EXPECT_EQ(a, b);

    // Spot-check the result is the handler's deterministic transform, so a
    // future handler change can't silently make both maps equal-but-wrong.
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(a.at(i), RecordingHandler::respond(i));
    }
}

// Model B with a single worker is still result-equivalent to Model A — the
// pool size is a concurrency/memory knob, not a correctness one.
TEST(HttpModelParityHarnessTests, singleWorkerPoolMatchesFiberPerConn) {
    const int n = 64;

    std::map<int, int64_t> a = runModelA(n);
    std::map<int, int64_t> b = runModelB(n, 1);

    EXPECT_EQ(static_cast<int>(a.size()), n);
    EXPECT_EQ(a, b);
}

// The result parity holds across a range of pool sizes — the model selection
// (which the NET-9.2 builder exposes) never alters what the handler computes.
TEST(HttpModelParityHarnessTests, parityHoldsAcrossPoolSizes) {
    const int n = 200;
    std::map<int, int64_t> a = runModelA(n);

    for (int pool : {1, 2, 4, 8, 16, 32}) {
        std::map<int, int64_t> b = runModelB(n, pool);
        EXPECT_EQ(static_cast<int>(b.size()), n) << "pool=" << pool;
        EXPECT_EQ(a, b) << "pool=" << pool;
    }
}
