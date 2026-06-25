//
// CP6f-2d unit 2 — the debug-only stop coordinator, tested in ISOLATION (no
// JIT, no carriers). The coordinator is the process-global rendezvous the
// carrier safepoints + scheduler hand-off will consult (units 3-6) and that
// DebugController drives: a `stop_requested` flag/generation, a parked/expected
// count guarded by a mutex+condvar, and a BOUNDED convergence wait so a stuck
// carrier can never hang the debugger (spec §2.1, §2.3, §4.1-4.3).
//
// These tests drive the coordinator directly with plain std::threads standing
// in for carriers — the real carrier/JIT wiring is units 3-6 (blocked on a
// JIT-runnable host). What is verifiable here: the flag set/clear + primary
// determinism, the parked/expected convergence, and that the bounded wait
// reports an un-quiesced quorum instead of hanging.
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

extern "C" {
    // Hot-path read: a single relaxed-atomic load (spec §4.2 / acceptance 2.3).
    int      __cajeta_stop_is_requested(void);
    // Flip 0->1 for this round; returns 1 iff THIS caller was the primary (the
    // first to set it this round), 0 if a stop was already in progress (§4.3).
    int      __cajeta_stop_request(void);
    // Resume-all: clear the flag, bump generation, wake every parked carrier.
    void     __cajeta_stop_clear(void);
    // Debugger declares how many carriers must park before inspection.
    void     __cajeta_stop_set_expected(int n);
    // A carrier parks: count itself, wake the convergence waiter, block until
    // the current stop round is cleared (returns immediately if not requested).
    void     __cajeta_stop_park(void);
    // Barrier: wait until parked>=expected OR timeout_ns elapses (<=0 == wait
    // forever). Returns the number still NOT parked (0 == fully quiesced).
    int      __cajeta_stop_wait_converged(long timeout_ns);
    int      __cajeta_stop_parked_count(void);
    int      __cajeta_stop_expected_count(void);
    unsigned __cajeta_stop_generation_get(void);
    void     __cajeta_stop_reset(void);   // test-only
}

namespace {
// Spin until `pred()` or `capMs` elapses — keeps the timing tests deterministic
// without sleeping a fixed (flaky) duration.
template <typename Pred>
bool spinUntil(Pred pred, int capMs = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(capMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return pred();
}
}  // namespace

TEST(DbgStopCoord, RequestSetsFlagPrimaryOnceClearResets) {
    __cajeta_stop_reset();
    EXPECT_EQ(__cajeta_stop_is_requested(), 0);          // idle off-path
    unsigned g0 = __cajeta_stop_generation_get();

    EXPECT_EQ(__cajeta_stop_request(), 1);               // first caller is primary
    EXPECT_EQ(__cajeta_stop_is_requested(), 1);
    EXPECT_EQ(__cajeta_stop_request(), 0);               // already requested -> secondary
    EXPECT_EQ(__cajeta_stop_generation_get(), g0 + 1);   // exactly one round opened

    __cajeta_stop_clear();
    EXPECT_EQ(__cajeta_stop_is_requested(), 0);
    EXPECT_EQ(__cajeta_stop_request(), 1);               // a fresh round can be primary again
    __cajeta_stop_clear();

    __cajeta_stop_reset();
}

TEST(DbgStopCoord, ParkedConvergesToExpectedThenResumeReleasesAll) {
    __cajeta_stop_reset();
    const int N = 6;
    __cajeta_stop_set_expected(N);
    EXPECT_EQ(__cajeta_stop_expected_count(), N);
    ASSERT_EQ(__cajeta_stop_request(), 1);

    std::vector<std::thread> carriers;
    for (int i = 0; i < N; ++i) {
        carriers.emplace_back([] {
            if (__cajeta_stop_is_requested()) __cajeta_stop_park();
        });
    }

    // Barrier returns 0 (fully quiesced) once every carrier has parked.
    EXPECT_EQ(__cajeta_stop_wait_converged(0), 0);
    EXPECT_EQ(__cajeta_stop_parked_count(), N);

    __cajeta_stop_clear();                               // resume-all
    for (auto& t : carriers) t.join();
    EXPECT_TRUE(spinUntil([] { return __cajeta_stop_parked_count() == 0; }));

    __cajeta_stop_reset();
}

TEST(DbgStopCoord, BoundedWaitReportsUnquiescedQuorumNoHang) {
    __cajeta_stop_reset();
    __cajeta_stop_set_expected(3);                       // demand 3...
    ASSERT_EQ(__cajeta_stop_request(), 1);

    std::vector<std::thread> carriers;                   // ...but only 2 ever park
    for (int i = 0; i < 2; ++i) {
        carriers.emplace_back([] { __cajeta_stop_park(); });
    }
    ASSERT_TRUE(spinUntil([] { return __cajeta_stop_parked_count() == 2; }));

    auto t0 = std::chrono::steady_clock::now();
    int missing = __cajeta_stop_wait_converged(50 * 1000 * 1000);   // 50ms bound
    auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(missing, 1);                               // 3 expected - 2 parked
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));   // honored the bound...
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500)); // ...and did NOT hang

    __cajeta_stop_clear();
    for (auto& t : carriers) t.join();
    __cajeta_stop_reset();
}

TEST(DbgStopCoord, ExactlyOnePrimaryUnderConcurrentRequests) {
    __cajeta_stop_reset();
    const int T = 32;
    std::atomic<int> primaries{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> racers;
    for (int i = 0; i < T; ++i) {
        racers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            if (__cajeta_stop_request() == 1) primaries.fetch_add(1, std::memory_order_relaxed);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : racers) t.join();

    EXPECT_EQ(primaries.load(), 1);                      // §4.3 determinism
    EXPECT_EQ(__cajeta_stop_is_requested(), 1);
    __cajeta_stop_clear();
    __cajeta_stop_reset();
}

// park() entered after the round was already cleared must NOT block (the
// carrier observed the flag, then resume raced in before it reached the mutex).
TEST(DbgStopCoord, ParkIsNoOpWhenNotRequested) {
    __cajeta_stop_reset();
    std::atomic<bool> returned{false};
    std::thread carrier([&] {
        __cajeta_stop_park();            // not requested -> returns immediately
        returned.store(true);
    });
    EXPECT_TRUE(spinUntil([&] { return returned.load(); }));
    carrier.join();
    EXPECT_EQ(__cajeta_stop_parked_count(), 0);
    __cajeta_stop_reset();
}
