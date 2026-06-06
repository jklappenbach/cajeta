//
// ConnectionLimiterHarness.h — NET-4.4 backpressure + connection-limit harness.
//
// The NET-4.4 backpressure mechanism
// (runtime/src/cajeta/net/ConnectionLimiter.cajeta + ConnectionLimits.cajeta
// + LoadShedPolicy.cajeta) is a piece of *pure admission logic* riding on a
// primitive that already exists (cajeta.threading.AtomicInt32): a
// semaphore-style permit pool, lock-free over a single atomic, that bounds
// the live (admitted-but-not-released) connection count to a configured cap
// — common to both server accept models (Model A fiber-per-connection,
// Model B shared-pool):
//
//   1. tryAdmit() — a non-blocking compare-and-set decrement loop: take a
//      permit iff one is free, else fail without parking. The heart of the
//      REFUSE load-shed policy (admit-or-shed) — the accept loop never
//      blocks under a flood.                                  (PermitPool)
//
//   2. release() — a clamped CAS increment loop: return a permit, but never
//      above the cap, so a stray double-release can't inflate the pool past
//      the configured maximum.                                (PermitPool)
//
//   3. the REFUSE vs. BLOCK load-shed policy: REFUSE sheds the (cap+1)th
//      connection (tryAdmit false → caller closes it); BLOCK parks until a
//      permit frees (awaitPermit, modeled here as a bounded spin).
//                                                       (LoadShedPolicy/admit)
//
// ## Why a C++ harness (not a `.cajeta` one)
//
// Identical rationale to NET-4.1's ServerLifecycleHarness.h: the
// cajeta-surface accept loop that drives a *live* ConnectionLimiter
// (Server.serve / SharedPoolServer.serve gating dispatch) needs the
// compiler-core net dispatch that is still being wired, so the full loop
// can't be JIT-run deterministically yet. But the admission LOGIC the
// limiter encodes is platform-independent and deterministically pinnable on
// its own — exactly what this header models, one level down, with
// std::atomic standing in for cajeta.threading.AtomicInt32. When the
// in-scheduler JIT `ServerTests.connectionCapShedsExcess` /
// `slowClientDoesNotStallOthers` acceptances land, this stays the native
// analog + the contract these fixtures freeze.
//
// The permit accounting + the REFUSE/BLOCK ordinals + the clamp semantics
// here MUST mirror runtime/src/cajeta/net/ConnectionLimiter.cajeta,
// LoadShedPolicy.cajeta, and ConnectionLimits.cajeta exactly — they are the
// executable spec for that pure-logic mechanism. Kept under test/ so
// production sources carry no test-only surface.
//
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace cajeta::net::testing {

    // -----------------------------------------------------------------------
    // load_shed — the NET-4.4 LoadShedPolicy ordinals, byte-identical to
    // runtime/src/cajeta/net/LoadShedPolicy.cajeta. Append-only.
    // -----------------------------------------------------------------------
    namespace load_shed {
        constexpr int32_t REFUSE = 0;   // admit-or-shed: close the surplus
        constexpr int32_t BLOCK  = 1;   // admit-or-wait: park until a slot frees
    }

    // -----------------------------------------------------------------------
    // ConnectionLimits — the NET-4.4 config defaults, byte-identical to
    // runtime/src/cajeta/net/ConnectionLimits.cajeta. The four backpressure
    // knobs common to both server models: the concurrent-connection cap, the
    // kernel listen backlog, and the per-connection read/write buffer caps.
    // -----------------------------------------------------------------------
    namespace conn_limits {
        constexpr int32_t DEFAULT_MAX_CONNECTIONS = 1024;
        constexpr int32_t DEFAULT_LISTEN_BACKLOG  = 128;
        constexpr int32_t DEFAULT_READ_BUFFER_CAP = 65536;
        constexpr int32_t DEFAULT_WRITE_BUFFER_CAP = 65536;
    }

    // -----------------------------------------------------------------------
    // PermitPool — a faithful native model of ConnectionLimiter.cajeta's
    // lock-free permit pool. Mirrors the exact CAS loops the Cajeta limiter
    // runs over its single AtomicInt32 `permits` cell:
    //
    //   * tryAdmit()  ≡ tryAdmit(): CAS-decrement iff a permit is free; never
    //                   blocks. capacity 0 ⇒ unbounded (always true).
    //   * release()   ≡ release(): CAS-increment, clamped at capacity so an
    //                   over-release can't grow the pool past the cap. cap 0
    //                   ⇒ no-op.
    //   * available() / inUse() / isUnbounded() ≡ the introspection surface.
    //
    // No sockets, no fibers — just the coordinating atomic, so the admission
    // accounting is pinned deterministically and independently of the
    // (partial) cajeta-surface accept loop.
    // -----------------------------------------------------------------------
    class PermitPool {
    public:
        // capacity 0 (or negative, clamped to 0) is the unbounded sentinel.
        explicit PermitPool(int32_t capacity)
            : capacity_(capacity < 0 ? 0 : capacity),
              permits_(capacity < 0 ? 0 : capacity) {}

        int32_t capacity() const { return capacity_; }
        bool isUnbounded() const { return capacity_ == 0; }

        // Non-blocking: take a permit iff one is free. True ⇒ took one
        // (caller must release); false ⇒ cap full. Unbounded ⇒ always true.
        bool tryAdmit() {
            if (capacity_ == 0) return true;
            for (;;) {
                int32_t free = permits_.load(std::memory_order_seq_cst);
                if (free <= 0) return false;
                if (permits_.compare_exchange_strong(free, free - 1)) return true;
                // lost the CAS to a concurrent admit/release — retry
            }
        }

        // Return a permit, clamped at capacity (drop a stray over-release).
        // Unbounded ⇒ no-op.
        void release() {
            if (capacity_ == 0) return;
            for (;;) {
                int32_t free = permits_.load(std::memory_order_seq_cst);
                if (free >= capacity_) return;   // already at cap — drop extra
                if (permits_.compare_exchange_strong(free, free + 1)) return;
                // lost the CAS — retry
            }
        }

        int32_t available() const { return permits_.load(std::memory_order_seq_cst); }

        int32_t inUse() const {
            if (capacity_ == 0) return 0;
            return capacity_ - permits_.load(std::memory_order_seq_cst);
        }

    private:
        const int32_t capacity_;
        std::atomic<int32_t> permits_;
    };

    // -----------------------------------------------------------------------
    // admitByPolicy — models ConnectionLimiter.admit(): REFUSE returns the
    // raw tryAdmit() (no wait); BLOCK spins tryAdmit() until it succeeds (the
    // native analog of awaitPermit's fiber park-with-backoff). Returns true
    // iff a permit was taken — only REFUSE on a full cap returns false.
    //
    // For BLOCK we cap the spin with `maxSpins` so a test can assert the
    // "blocks until a slot frees" behavior without an unbounded busy-wait if
    // a permit never frees (it would in the live system; here the test frees
    // one from another thread). maxSpins <= 0 ⇒ unbounded (spin to success).
    // -----------------------------------------------------------------------
    inline bool admitByPolicy(PermitPool& pool, int32_t policy, int64_t maxSpins) {
        if (policy == load_shed::BLOCK) {
            int64_t spins = 0;
            while (!pool.tryAdmit()) {
                if (maxSpins > 0 && ++spins >= maxSpins) return false;
                std::this_thread::yield();
            }
            return true;
        }
        return pool.tryAdmit();   // REFUSE
    }

} // namespace cajeta::net::testing
