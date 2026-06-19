//
// FiberPerConnHarness.h — NET-4.2 fiber-per-connection accept model harness.
//
// NET-4.1 (ServerLifecycleHarness.h) pins the server *core*: the
// NEW→RUNNING→DRAINING→STOPPED lifecycle and the in-flight drain counter.
// NET-4.2 is the **fiber-per-connection dispatch model** that rides that
// core — the `Server.dispatch` / `Server.serveConnection` seam in
// runtime/src/cajeta/io/net/Server.cajeta. Its distinct, testable contract
// (over and above the bare lifecycle the NET-4.1 harness already freezes):
//
//   1. **One fiber per accepted socket.** The accept loop spawns exactly
//      one connection fiber per accepted connection — a 1:1 accept→spawn
//      mapping (Model A; cf. Model B's bounded shared pool, NET-4.3, where
//      that mapping is N-accepts→pool-of-K-workers). Dispatching `n`
//      connections spawns `n` handler fibers, no more, no fewer.
//
//   2. **Structured concurrency: the scope owns every connection fiber.**
//      `Server.serve` issues each `spawn` from inside a single
//      `scope { ... }`, so the scope's closing brace joins every still-
//      running connection fiber before `serve` returns. A `shutdown` that
//      stopped accepting therefore cannot leave a handler orphaned — the
//      drain that polls in-flight to zero is *backed by* the scope-join,
//      not a substitute for it.
//
//   3. **True concurrency, not serialized dispatch.** The point of a
//      fiber-per-connection is that slow/parked handlers run concurrently:
//      K connections that each block reach an in-flight peak of K *at the
//      same time*, rather than draining one-at-a-time. (This is the
//      property that distinguishes Model A from a single-worker loop, and
//      the one the JIT `ServerTests.fiberPerConnServes500Clients`
//      acceptance proves with 500 live clients.)
//
//   4. **Handler isolation.** A connection handler that throws is isolated
//      to its own fiber: it still drops its in-flight count (the `finally`
//      in `serveConnection`), and it never tears down the accept loop or
//      its sibling connection fibers — they keep running and complete.
//
// ## Why a C++ harness (not a `.cajeta` one)
//
// Identical rationale to NET-4.1 (ServerLifecycleHarness.h) and the
// NET-13.x harnesses: the cajeta-surface socket lowering the *full*
// `Server.serve` accept loop needs (NET-1.3/1.4, "partial: needs
// compiler-core net dispatch") is still being wired, so the live loop
// cannot be JIT-run deterministically yet. But the dispatch *model* the
// fiber-per-connection seam encodes — 1:1 spawn, scope-join drain,
// concurrent in-flight peak, throw isolation — is platform-independent and
// deterministically pinnable one level down, with std::thread standing in
// for a connection fiber and the SAME DrainCounter / ServerLifecycle the
// NET-4.1 harness models the core with (composed verbatim here, so the two
// harnesses cannot drift). When the in-scheduler JIT `ServerTests.*` suite
// lands, this stays its native analog and the contract those tests reuse.
//
// The dispatch rules here MUST mirror runtime/src/cajeta/io/net/Server.cajeta
// (`dispatch`, `serveConnection`, the `serve` scope) exactly. Kept under
// test/ so production sources carry no test-only surface.
//
#pragma once

#include "net/ServerLifecycleHarness.h"   // ServerLifecycle, DrainCounter

#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cajeta::net::testing {

    // -----------------------------------------------------------------------
    // ConnFiber — a connection handler running on its own "fiber" (an OS
    // thread, standing in for the spawned Cajeta fiber). Models exactly the
    // body of Server.serveConnection: run the handler, then drop the
    // in-flight count in a `finally` — so a handler that THROWS still
    // decrements and its exception is isolated to this fiber (never escapes
    // to the accept loop).
    //
    //   handler : the (TcpStream)->void connection callback, modeled as a
    //             nullary std::function (the "stream" it owns is implicit in
    //             the harness — what we pin is the dispatch/accounting, not
    //             the socket I/O, which the loopback fixtures cover).
    //   inflight: the server's shared DrainCounter (NET-4.1), bumped by
    //             dispatch BEFORE the spawn and dropped here on return.
    //   threw   : set true iff the handler raised — the test-observable
    //             record that the throw was caught + isolated, not
    //             propagated.
    // -----------------------------------------------------------------------
    struct ConnFiber {
        std::thread thread;
        std::atomic<bool> threw{false};
        std::atomic<bool> done{false};
    };

    // -----------------------------------------------------------------------
    // FiberPerConnDispatcher — the NET-4.2 dispatch model. Mirrors
    // Server.dispatch + Server.serveConnection running inside Server.serve's
    // owning `scope`:
    //
    //   * dispatch(handler): bump in-flight, then spawn one fiber that runs
    //     the handler and drops in-flight in a `finally`. One call ⇒ one
    //     fiber (the 1:1 accept→spawn mapping).
    //   * joinScope():       the `serve` scope's closing brace — join every
    //     connection fiber. After it returns, in-flight is necessarily zero
    //     (every `finally` ran), which is the structured-concurrency
    //     property a graceful shutdown's drain rides on.
    //
    // The dispatcher owns the fibers (the scope owns them in Cajeta); it
    // never double-runs a handler and never lets a handler throw escape the
    // spawned fiber. `spawned()` is the count of fibers ever dispatched —
    // the 1:1 mapping assertion. `peakInflight()` records the high-water
    // mark of concurrently-live handlers — the true-concurrency assertion.
    // -----------------------------------------------------------------------
    class FiberPerConnDispatcher {
    public:
        // The shared in-flight counter is the NET-4.1 DrainCounter — composed,
        // not re-modeled, so the two harnesses share one source of truth.
        DrainCounter& inflight() { return inflight_; }

        // Total connection fibers ever dispatched (the 1:1 accept→spawn count).
        int spawned() const { return spawned_.load(std::memory_order_seq_cst); }

        // High-water mark of concurrently-live handler fibers — proves the
        // model runs handlers concurrently (peak ≈ K for K parked handlers),
        // not one-at-a-time.
        int peakInflight() const { return peak_.load(std::memory_order_seq_cst); }

        // Dispatch one accepted connection: bump in-flight (BEFORE the spawn,
        // exactly as Server.dispatch does), then spawn one fiber running the
        // handler with the serveConnection `finally`-decrement + throw
        // isolation. Returns a reference to the owned fiber record.
        ConnFiber& dispatch(std::function<void()> handler) {
            spawned_.fetch_add(1, std::memory_order_seq_cst);
            inflight_.enter();                          // dispatch: bump BEFORE spawn
            recordPeak();                               // peak observed at dispatch
            fibers_.push_back(std::make_unique<ConnFiber>());
            ConnFiber& f = *fibers_.back();
            f.thread = std::thread([this, &f, handler = std::move(handler)]() {
                // ≡ Server.serveConnection: run the handler, drop in-flight in a
                // `finally` so a throwing handler still decrements + is isolated.
                try {
                    recordPeak();                       // peak also sampled once live
                    handler();
                } catch (...) {
                    f.threw.store(true, std::memory_order_seq_cst);
                    // swallowed: the exception is isolated to this fiber and
                    // never reaches the accept loop or its sibling fibers.
                }
                inflight_.leave();                      // the `finally` decrement
                f.done.store(true, std::memory_order_seq_cst);
            });
            return f;
        }

        // The `serve` scope's closing brace: join every connection fiber.
        // After this returns, in-flight is zero (every `finally` ran) — the
        // structured-concurrency guarantee a graceful shutdown awaits.
        void joinScope() {
            for (auto& f : fibers_) {
                if (f->thread.joinable()) f->thread.join();
            }
        }

        ~FiberPerConnDispatcher() { joinScope(); }

    private:
        void recordPeak() {
            int now = inflight_.count();
            int prev = peak_.load(std::memory_order_seq_cst);
            while (now > prev &&
                   !peak_.compare_exchange_weak(prev, now, std::memory_order_seq_cst)) {
                // prev refreshed by the failed CAS; retry until we win or
                // someone else already recorded a >= mark.
            }
        }

        DrainCounter inflight_;                          // NET-4.1 shared counter
        std::atomic<int> spawned_{0};
        std::atomic<int> peak_{0};
        std::vector<std::unique_ptr<ConnFiber>> fibers_;
    };

} // namespace cajeta::net::testing
