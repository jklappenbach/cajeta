//
// ServerLifecycleHarness.h — NET-4.1 server-core lifecycle + drain harness.
//
// The NET-4.1 `Server` core (runtime/src/cajeta/net/Server.cajeta) is two
// subtle, easy-to-regress pieces of *pure logic* riding on primitives that
// already exist (TcpListener, the reactor, AtomicInt32, Duration):
//
//   1. A monotone lifecycle state machine — NEW → RUNNING → DRAINING →
//      STOPPED — driven by a single atomic the accept loop and a
//      concurrent shutdown coordinate on via compare-and-set. The two
//      invariants it buys: `serve()` is start-once (a second serve is a
//      no-op) and `shutdown()` is a latch (the RUNNING→DRAINING CAS fires
//      at most once, so the accept loop stops pulling new connections and
//      a second shutdown is a no-op).                       (ServerState)
//
//   2. Graceful-drain accounting — each dispatched connection bumps an
//      in-flight counter before its handler runs and drops it in a
//      `finally` (so a throwing handler still decrements); shutdown polls
//      that counter to zero, bounded by a deadline, returning whether the
//      drain completed cleanly or the deadline forced the stop.
//                                                          (DrainCounter)
//
// ## Why a C++ harness (not a `.cajeta` one)
//
// Identical rationale to NET-13.1 / NET-13.4 (LoopbackFixtures.h,
// ReactorHarness.h): the cajeta-surface socket lowering the full
// `Server.serve()` accept loop needs (NET-1.3/1.4 "partial: needs
// compiler-core net dispatch") is still being wired, so the *full* loop
// cannot be JIT-run deterministically yet. But the lifecycle + drain
// LOGIC the core encodes is platform-independent and deterministically
// pinnable on its own — exactly what this header models, one level down,
// with std::atomic standing in for cajeta.threading.AtomicInt32. When the
// in-scheduler JIT `ServerTests.*` suite the plan names lands, this stays
// the native analog + the contract these fixtures freeze.
//
// The state ordinals + transition rules + drain semantics here MUST mirror
// runtime/src/cajeta/net/ServerState.cajeta and Server.cajeta exactly —
// they are the executable spec for that pure-logic core. Kept under test/
// so production sources carry no test-only surface.
//
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace cajeta::net::testing {

    // -----------------------------------------------------------------------
    // ServerState — the NET-4.1 lifecycle ordinals, byte-identical to
    // runtime/src/cajeta/net/ServerState.cajeta. Append-only; never renumber.
    // -----------------------------------------------------------------------
    namespace server_state {
        constexpr int32_t NEW      = 0;
        constexpr int32_t RUNNING  = 1;
        constexpr int32_t DRAINING = 2;
        constexpr int32_t STOPPED  = 3;
    }

    // -----------------------------------------------------------------------
    // ServerLifecycle — a faithful native model of the Server.cajeta state
    // machine. Mirrors the exact transitions the Cajeta core runs:
    //
    //   * tryStartServing()  ≡ serve(): CAS NEW → RUNNING. Returns true iff
    //                          this call started the loop (the start-once
    //                          guard); false if already serving / past it.
    //   * isAccepting()      ≡ the accept loop's `state == RUNNING` test.
    //   * requestShutdown()  ≡ shutdown() step 1: CAS RUNNING → DRAINING.
    //                          Returns true iff this call latched the drain
    //                          (fires at most once → shutdown idempotent).
    //   * finishStop()       ≡ shutdown() step 4: store STOPPED.
    //
    // No sockets, no fibers — just the coordinating atomic, so the state
    // transitions are pinned deterministically and independently of the
    // (partial) cajeta-surface accept loop.
    // -----------------------------------------------------------------------
    class ServerLifecycle {
    public:
        ServerLifecycle() : state_(server_state::NEW) {}

        int32_t state() const { return state_.load(std::memory_order_seq_cst); }

        // CAS NEW → RUNNING. True iff this call performed the transition.
        bool tryStartServing() {
            int32_t expected = server_state::NEW;
            return state_.compare_exchange_strong(expected, server_state::RUNNING);
        }

        // The accept loop continues while — and only while — RUNNING.
        bool isAccepting() const { return state() == server_state::RUNNING; }

        // CAS RUNNING → DRAINING. True iff this call latched the drain.
        bool requestShutdown() {
            int32_t expected = server_state::RUNNING;
            return state_.compare_exchange_strong(expected, server_state::DRAINING);
        }

        void finishStop() { state_.store(server_state::STOPPED); }

    private:
        std::atomic<int32_t> state_;
    };

    // -----------------------------------------------------------------------
    // DrainCounter — the graceful-drain in-flight accounting + deadline wait,
    // modeling Server.cajeta's `inflight` AtomicInt32 + drainInflight(). A
    // dispatched connection calls enter() before its handler and leave() in a
    // finally; drainTo Zero() polls to zero bounded by a nanosecond deadline,
    // returning true on a clean drain, false if the deadline forced it.
    // -----------------------------------------------------------------------
    class DrainCounter {
    public:
        // Dispatch bumps the count before running the handler.
        void enter() { count_.fetch_add(1, std::memory_order_seq_cst); }

        // The handler's `finally` drops it — runs even on a throwing handler.
        void leave() { count_.fetch_sub(1, std::memory_order_seq_cst); }

        int32_t count() const { return count_.load(std::memory_order_seq_cst); }

        // Poll the in-flight count to zero, bounded by `deadlineNanos` of
        // wall-clock budget. Mirrors Server.drainInflight: a 0-budget drain
        // returns immediately (true iff already at zero), and the poll backs
        // off (100us → 10ms cap) so a quiescent drain is cheap. Returns true
        // on a clean drain, false if the budget expired with work in flight.
        bool drainToZero(int64_t deadlineNanos) {
            using clock = std::chrono::steady_clock;
            const auto start = clock::now();
            int64_t backoff = 100000;   // 100us, doubling to a 10ms cap
            while (count() > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         clock::now() - start)
                                         .count();
                if (elapsed >= deadlineNanos) return false;
                std::this_thread::sleep_for(std::chrono::nanoseconds(backoff));
                if (backoff < 10000000) backoff *= 2;
            }
            return true;
        }

    private:
        std::atomic<int32_t> count_{0};
    };

} // namespace cajeta::net::testing
