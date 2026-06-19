//
// ConnectionLimiterHarnessTests.cpp — NET-4.4 backpressure + connection cap.
//
// Pins the pure-logic contract of the NET-4.4 backpressure mechanism
// (runtime/src/cajeta/io/net/ConnectionLimiter.cajeta + ConnectionLimits.cajeta
// + LoadShedPolicy.cajeta), modeled one level down via
// ConnectionLimiterHarness.h (std::atomic standing in for
// cajeta.concurrent.AtomicInt32), in the deterministic, scheduler-free
// posture the other net harnesses use (ServerLifecycleHarness.h) while the
// cajeta-surface accept loop that drives a live limiter is still being
// wired. Three groups:
//
//   * the permit pool — tryAdmit / release accounting, the cap bound, the
//     release-clamp that defends against over-release, and the unbounded
//     (cap 0) bypass;
//   * the load-shed policy — REFUSE sheds the (cap+1)th connection
//     (`ServerTests.connectionCapShedsExcess`), BLOCK waits for a freed
//     permit; and
//   * the cross-connection isolation property — one held permit (a slow
//     client) never blocks admit/release on the *other* permits
//     (`ServerTests.slowClientDoesNotStallOthers`).
//
// The full in-scheduler version (a live accept loop gating dispatch on the
// limiter, shedding the (cap+1)th loopback client) is the JIT
// `ServerTests.*` suite the plan names; this is its native analog and the
// contract those tests reuse.
//

#include "gtest/gtest.h"

#include "net/ConnectionLimiterHarness.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace cajeta::net::testing;
namespace ls = cajeta::net::testing::load_shed;
namespace cl = cajeta::net::testing::conn_limits;

// --- config: the documented defaults are byte-stable ----------------------
TEST(ConnectionLimiterHarnessTests, configDefaultsAreStable) {
    EXPECT_EQ(1024, cl::DEFAULT_MAX_CONNECTIONS);
    EXPECT_EQ(128, cl::DEFAULT_LISTEN_BACKLOG);
    EXPECT_EQ(65536, cl::DEFAULT_READ_BUFFER_CAP);
    EXPECT_EQ(65536, cl::DEFAULT_WRITE_BUFFER_CAP);
    // The LoadShedPolicy ordinals are an append-only contract.
    EXPECT_EQ(0, ls::REFUSE);
    EXPECT_EQ(1, ls::BLOCK);
}

// --- pool: a fresh pool has `capacity` free permits, none in use ----------
TEST(ConnectionLimiterHarnessTests, freshPoolIsAllFree) {
    PermitPool pool(4);
    EXPECT_EQ(4, pool.capacity());
    EXPECT_EQ(4, pool.available());
    EXPECT_EQ(0, pool.inUse());
    EXPECT_FALSE(pool.isUnbounded());
}

// --- pool: each admit takes one permit, each release returns one ----------
TEST(ConnectionLimiterHarnessTests, admitTakesReleaseReturns) {
    PermitPool pool(3);
    EXPECT_TRUE(pool.tryAdmit());
    EXPECT_EQ(2, pool.available());
    EXPECT_EQ(1, pool.inUse());
    EXPECT_TRUE(pool.tryAdmit());
    EXPECT_TRUE(pool.tryAdmit());
    EXPECT_EQ(0, pool.available());
    EXPECT_EQ(3, pool.inUse());

    pool.release();
    EXPECT_EQ(1, pool.available());
    EXPECT_EQ(2, pool.inUse());
}

// --- cap: the (cap+1)th admit is refused (REFUSE shed) --------------------
// The connectionCapShedsExcess acceptance, one level down: with the cap
// full, the non-blocking tryAdmit fails — the accept loop sheds (closes) the
// surplus connection rather than admitting it.
TEST(ConnectionLimiterHarnessTests, capRefusesExcess) {
    PermitPool pool(2);
    EXPECT_TRUE(pool.tryAdmit());            // 1st — admitted
    EXPECT_TRUE(pool.tryAdmit());            // 2nd (== cap) — admitted
    EXPECT_EQ(0, pool.available());

    EXPECT_FALSE(pool.tryAdmit())            // (cap+1)th — SHED
        << "the (cap+1)th connection must be refused under a full cap";
    EXPECT_EQ(2, pool.inUse())
        << "a refused admission takes no permit — in-use stays at the cap";

    // Freeing a permit re-opens admission for the next connection.
    pool.release();
    EXPECT_TRUE(pool.tryAdmit())
        << "once a permit frees, the next connection is admitted again";
}

// --- release-clamp: an over-release can't inflate the pool past the cap ----
// ConnectionLimiter.release clamps at capacity, so a stray double-release
// (or a release with no prior admit) never grows the free count beyond the
// configured maximum — which would otherwise let the limiter admit *more*
// than the cap.
TEST(ConnectionLimiterHarnessTests, releaseClampsAtCapacity) {
    PermitPool pool(2);
    EXPECT_EQ(2, pool.available());

    pool.release();   // stray over-release on a full pool
    pool.release();
    EXPECT_EQ(2, pool.available())
        << "release must clamp at capacity — never inflate the permit pool";

    // The cap still holds exactly: 2 admits, then the 3rd is refused.
    EXPECT_TRUE(pool.tryAdmit());
    EXPECT_TRUE(pool.tryAdmit());
    EXPECT_FALSE(pool.tryAdmit())
        << "after the clamped over-release the cap is still exactly 2";
}

// --- unbounded: a cap of 0 bypasses the gate entirely ----------------------
TEST(ConnectionLimiterHarnessTests, unboundedPoolNeverRefuses) {
    PermitPool pool(0);
    EXPECT_TRUE(pool.isUnbounded());
    for (int i = 0; i < 10000; ++i) {
        EXPECT_TRUE(pool.tryAdmit()) << "an unbounded pool admits unconditionally";
    }
    EXPECT_EQ(0, pool.inUse()) << "unbounded pools track no permits";
    pool.release();   // no-op on an unbounded pool
    EXPECT_EQ(0, pool.available());
}

// --- a negative cap degrades safely to unbounded (no-cap), not all-refuse --
TEST(ConnectionLimiterHarnessTests, negativeCapacityIsUnbounded) {
    PermitPool pool(-5);
    EXPECT_TRUE(pool.isUnbounded())
        << "a misconfigured negative cap clamps to 0 (unbounded), not all-refuse";
    EXPECT_TRUE(pool.tryAdmit());
}

// --- policy REFUSE: admit returns tryAdmit verbatim, never waits -----------
TEST(ConnectionLimiterHarnessTests, refusePolicyDoesNotWait) {
    PermitPool pool(1);
    EXPECT_TRUE(admitByPolicy(pool, ls::REFUSE, 0));    // 1st — admitted
    EXPECT_FALSE(admitByPolicy(pool, ls::REFUSE, 0))    // 2nd — refused, no wait
        << "REFUSE returns immediately when the cap is full";
}

// --- policy BLOCK: admit waits until a permit frees ------------------------
// BLOCK parks (here: spins) until a slot frees. With the cap full, a
// background thread releases a permit shortly after; the blocked admit then
// succeeds — the admit-or-wait semantics.
TEST(ConnectionLimiterHarnessTests, blockPolicyWaitsForPermit) {
    PermitPool pool(1);
    ASSERT_TRUE(pool.tryAdmit());            // cap now full
    EXPECT_EQ(0, pool.available());

    std::atomic<bool> admitted{false};
    std::thread waiter([&] {
        // Unbounded spin (maxSpins 0) — must succeed once the permit frees.
        admitted.store(admitByPolicy(pool, ls::BLOCK, 0));
    });

    // Give the waiter a moment to confirm it is genuinely blocked.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(admitted.load()) << "BLOCK must park while the cap is full";

    pool.release();                          // free the permit
    waiter.join();
    EXPECT_TRUE(admitted.load()) << "BLOCK must admit once a permit frees";
}

// --- BLOCK with a bounded spin gives up if no permit ever frees (model) ----
TEST(ConnectionLimiterHarnessTests, blockBoundedSpinGivesUp) {
    PermitPool pool(1);
    ASSERT_TRUE(pool.tryAdmit());            // full; nothing will free it
    EXPECT_FALSE(admitByPolicy(pool, ls::BLOCK, 1000))
        << "a bounded-spin BLOCK that never sees a free permit reports failure";
}

// --- isolation: one held permit never blocks the others --------------------
// The slowClientDoesNotStallOthers property: a single connection that holds
// its permit indefinitely (a slow client) must NOT prevent the remaining
// permits from being admitted and released. Hold permit #1 for the whole
// test; the other (cap-1) permits must still cycle freely.
TEST(ConnectionLimiterHarnessTests, oneHeldPermitDoesNotStallOthers) {
    constexpr int32_t cap = 4;
    PermitPool pool(cap);

    ASSERT_TRUE(pool.tryAdmit());            // the "slow client" — never released
    EXPECT_EQ(cap - 1, pool.available());

    // The other three permits cycle many times without ever touching the
    // held one — a slow client stalls only itself.
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < cap - 1; ++i) ASSERT_TRUE(pool.tryAdmit());
        EXPECT_EQ(0, pool.available()) << "the non-slow permits are all in use";
        EXPECT_FALSE(pool.tryAdmit()) << "and the cap still holds with the slow one pinned";
        for (int i = 0; i < cap - 1; ++i) pool.release();
        EXPECT_EQ(cap - 1, pool.available()) << "they all freed; the slow permit stays held";
    }
    // The slow client is still counted — it never starved the others, nor
    // did the others' churn ever free its permit.
    EXPECT_EQ(1, pool.inUse());
}

// --- concurrency: N racing admits never over-admit past the cap -----------
// The CAS admit/release must hold under contention: spin up far more threads
// than permits, each tries once; the number that succeed is EXACTLY the cap
// (never more — that would be an over-admit), and in-use lands at the cap.
TEST(ConnectionLimiterHarnessTests, concurrentAdmitsNeverExceedCap) {
    constexpr int32_t cap = 16;
    constexpr int threads = 128;
    PermitPool pool(cap);

    std::atomic<int> admitted{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i) {
        ts.emplace_back([&] {
            if (pool.tryAdmit()) admitted.fetch_add(1);
        });
    }
    for (auto& t : ts) t.join();

    EXPECT_EQ(cap, admitted.load())
        << "exactly `cap` admits win — the CAS never over-admits under a race";
    EXPECT_EQ(cap, pool.inUse());
    EXPECT_EQ(0, pool.available());
    EXPECT_FALSE(pool.tryAdmit()) << "the pool is exactly full, no permit to spare";
}

// --- concurrency: balanced admit+release leaves the pool exactly full -----
// Every winning admit is paired with a release (the handler `finally`
// contract). After a storm of paired admit/release, the pool returns to all
// permits free — no permit leaked, none duplicated.
TEST(ConnectionLimiterHarnessTests, balancedAdmitReleaseConserves) {
    constexpr int32_t cap = 8;
    constexpr int threads = 64;
    PermitPool pool(cap);

    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i) {
        ts.emplace_back([&] {
            for (int k = 0; k < 1000; ++k) {
                if (pool.tryAdmit()) {
                    // hold briefly, then release — the paired finally
                    pool.release();
                }
            }
        });
    }
    for (auto& t : ts) t.join();

    EXPECT_EQ(cap, pool.available())
        << "balanced admit/release conserves the permit count — none leaked";
    EXPECT_EQ(0, pool.inUse());
}
