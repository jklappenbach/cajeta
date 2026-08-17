// lazy-codegen 2.1.4 / 2.2.2 — the compiler gate.
//
// ORC serializes tryToGenerate per GENERATOR OBJECT (Core.h: mutex + InUse +
// PendingLookups). A process holds several generators — one per host/session,
// and the test binary holds many — while the compiler's global state
// (canonicalMap, active module, substitution stack) is one per process. The
// gate closes that gap, and these tests OBSERVE the discipline rather than
// trust either mutex: max-threads-inside is counted, not inferred from timing.

#include "gtest/gtest.h"

#include "cajeta/jit/CajetaDefinitionGenerator.h"

#include <atomic>
#include <thread>
#include <vector>

using cajeta::CompilerGate;

namespace {
constexpr int kThreads = 8;
constexpr int kIterations = 200;
} // namespace

// 2.2.2 — never two threads inside. Deterministic: the mutex either exists on
// this path or it does not; no scheduling luck can make a missing lock pass.
TEST(CompilerGateTests, admitsOneThreadAtATime) {
    auto& gate = CompilerGate::instance();
    gate.resetObservation();

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIterations; ++i) {
                gate.run([] {
                    EXPECT_TRUE(CompilerGate::heldByThisThread());
                });
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(gate.maxThreadsObserved(), 1u)
        << "two threads were inside the compiler at once";
}

// The control: the same observation run UNGATED, with every thread held inside
// until all have arrived, must report kThreads. Without this, admitsOneThread
// could pass because the counter is broken, not because the gate works.
TEST(CompilerGateTests, observationDetectsConcurrencyWhenUngated) {
    auto& gate = CompilerGate::instance();
    gate.resetObservation();

    std::atomic<int> arrived{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            gate.runUngatedForTest([&] {
                arrived.fetch_add(1);
                while (arrived.load() < kThreads) std::this_thread::yield();
            });
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(gate.maxThreadsObserved(), static_cast<size_t>(kThreads));
}

// Same-thread re-entry must be legal and still count as ONE thread: emitting a
// body can trigger a nested lookup on the same thread, and a non-recursive
// gate would deadlock the session at the first cascade (spec 3.4).
TEST(CompilerGateTests, sameThreadReentryIsAllowed) {
    auto& gate = CompilerGate::instance();
    gate.resetObservation();

    bool innerRan = false;
    gate.run([&] {
        gate.run([&] {
            innerRan = true;
            EXPECT_TRUE(CompilerGate::heldByThisThread());
        });
    });

    EXPECT_TRUE(innerRan);
    EXPECT_FALSE(CompilerGate::heldByThisThread());
    EXPECT_EQ(gate.maxThreadsObserved(), 1u);
}
