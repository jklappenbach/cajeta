//
// CP6f-2d unit 3 — cross-carrier convergence at safepoints + scheduler
// hand-off (spec §2.2, §5.1), tested WITHOUT the JIT. We drive the REAL carrier
// pool from C: __cajeta_task_run spawns fibers whose bodies are plain C
// functions that hammer __cajeta_dbg_safepoint, and a test trampoline bridges
// the runtime safepoint to a real DebugController (exactly as CajetaJitHost
// does). One fiber hits an ARMED loc; the assertion is stop-the-world: once it
// parks, a counter advanced by the OTHER fibers must not move until resume.
//
// CAJETA_CARRIERS is pinned to the fiber count so every fiber gets its own
// carrier — the fiber bodies are non-yielding compute loops, so with fewer
// carriers than fibers the armed fiber could starve and never hit its
// breakpoint. (The real JIT programs yield; this is a harness constraint.)
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <unistd.h>

// POSIX setenv is absent on mingw; shim onto _putenv_s (setting to "" also unsets).
#if defined(_WIN32)
#include <stdlib.h>
static inline int setenv(const char* k, const char* v, int) { return _putenv_s(k, v ? v : ""); }
static inline int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif

#include "cajeta/dbg/DebugController.h"

extern "C" {
    typedef void (*cajeta_task_trampoline_fn)(void* arg);
    typedef void (*cajeta_dbg_handler_fn)(int32_t loc_id, int fiber_id, void* frame_top);
    void __cajeta_task_run(void* arg, cajeta_task_trampoline_fn tramp, void** slot);
    void __cajeta_task_shutdown(void);
    void __cajeta_dbg_safepoint(int32_t loc_id);
    void __cajeta_dbg_set_safepoint_handler(cajeta_dbg_handler_fn fn);
    int  __cajeta_stop_parked_count(void);
    void __cajeta_stop_reset(void);
}

namespace {
constexpr int32_t kArmedLoc = 42;
constexpr int32_t kPlainLoc = 7;
constexpr int kFibers = 4;   // 3 plain + 1 armed; carriers pinned to match

cajeta::dbg::DebugController* g_ctrl = nullptr;
std::atomic<long> g_counter{0};
std::atomic<bool> g_quit{false};
std::atomic<bool> g_stuckRunning{false};   // stuck fiber is in its native call
std::atomic<int> g_armedReady{0};          // start-gate for the two-armed race

extern "C" void testTrampoline(int32_t loc, int fid, void* ft) {
    if (g_ctrl) g_ctrl->onSafepoint(loc, static_cast<long>(fid), ft);
}

// Stands in for the runtime throw chokepoint calling the armed-exception
// handler (CP6f-3) — drives onException directly so unit 6 tests the quiesce
// integration, not the unwinder.
void throwingFiber(void*) {
    if (g_ctrl) g_ctrl->onException(reinterpret_cast<void*>(0xDEAD), 0, nullptr);
    while (!g_quit.load(std::memory_order_relaxed)) {
        __cajeta_dbg_safepoint(kPlainLoc);
        g_counter.fetch_add(1, std::memory_order_relaxed);
    }
}

void plainFiber(void*) {
    while (!g_quit.load(std::memory_order_relaxed)) {
        __cajeta_dbg_safepoint(kPlainLoc);          // never armed
        g_counter.fetch_add(1, std::memory_order_relaxed);
    }
}
void armedFiber(void*) {
    __cajeta_dbg_safepoint(kArmedLoc);              // armed -> opens the stop
    while (!g_quit.load(std::memory_order_relaxed)) {
        __cajeta_dbg_safepoint(kPlainLoc);
        g_counter.fetch_add(1, std::memory_order_relaxed);
    }
}

// Simulates a carrier blocked in a native call for the whole stop window: it
// never reaches a safepoint NOR returns to the carrier loop (which would let it
// idle-park and count as quiesced), so it can never park — the barrier must
// time out and flag it (§5.4). Stays "in native" until teardown sets g_quit.
void nativeStuckFiber(void*) {
    g_stuckRunning.store(true);                     // handshake: now in native
    while (!g_quit.load(std::memory_order_relaxed)) {
        usleep(20 * 1000);                          // perpetually mid-native-call
    }
}

// Armed fiber that waits until the stuck fiber is provably in its native call
// before triggering the stop — so the stuck carrier is genuinely running (not
// idle/un-dispatched, which would idle-park and count as quiesced). Each fiber
// has its own pinned carrier, so the spin doesn't starve the stuck one.
void armedAfterStuckFiber(void*) {
    while (!g_stuckRunning.load(std::memory_order_acquire)) { /* spin */ }
    __cajeta_dbg_safepoint(kArmedLoc);
    while (!g_quit.load(std::memory_order_relaxed)) {
        __cajeta_dbg_safepoint(kPlainLoc);
        g_counter.fetch_add(1, std::memory_order_relaxed);
    }
}

// Hits a caller-chosen armed loc exactly once, then loops a non-armed loc. Two
// of these on distinct armed locs exercise the near-simultaneous-stop race.
void armedOnceFiber(void* arg) {
    int32_t loc = static_cast<int32_t>(reinterpret_cast<intptr_t>(arg));
    // Start-gate: don't hit the armed loc until BOTH armed fibers are running,
    // so they engage the same stop round (otherwise an idle carrier can satisfy
    // the barrier before the second fiber reaches its breakpoint, and it then
    // fires as a spurious second primary after resume).
    g_armedReady.fetch_add(1, std::memory_order_acq_rel);
    while (g_armedReady.load(std::memory_order_acquire) < 2) { /* spin */ }
    __cajeta_dbg_safepoint(loc);
    while (!g_quit.load(std::memory_order_relaxed)) {
        __cajeta_dbg_safepoint(kPlainLoc);
        g_counter.fetch_add(1, std::memory_order_relaxed);
    }
}

template <typename Pred>
bool spinUntil(Pred p, int capMs = 4000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(capMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::yield();
    }
    return p();
}
}  // namespace

TEST(DbgCarrierQuiesce, BreakpointQuiescesAllCarriers) {
    __cajeta_stop_reset();
    g_counter.store(0);
    g_quit.store(false);
    setenv("CAJETA_CARRIERS", "4", 1);

    cajeta::dbg::DebugController controller;
    g_ctrl = &controller;
    controller.arm(kArmedLoc);
    __cajeta_dbg_set_safepoint_handler(testTrampoline);

    void* slots[kFibers] = {nullptr};
    for (int i = 0; i < kFibers - 1; ++i) __cajeta_task_run(nullptr, plainFiber, &slots[i]);
    __cajeta_task_run(nullptr, armedFiber, &slots[kFibers - 1]);

    // Debugger side: wait for the breakpoint to park the primary.
    cajeta::dbg::StopEvent ev;
    ASSERT_TRUE(controller.waitForStop(ev, std::chrono::seconds(5)))
        << "breakpoint never parked — armed fiber may have starved";
    EXPECT_EQ(ev.locId, kArmedLoc);

    // Convergence: the other 3 carriers reach their next safepoint and park as
    // secondaries. parked_count counts them (the primary is parked in the
    // DebugController, not the coordinator).
    ASSERT_TRUE(spinUntil([] { return __cajeta_stop_parked_count() >= kFibers - 1; }))
        << "carriers did not converge; parked=" << __cajeta_stop_parked_count();

    // Stop-the-world: no fiber advances the counter while stopped.
    long c1 = g_counter.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    long c2 = g_counter.load();
    EXPECT_EQ(c1, c2) << "a fiber advanced while the program was stopped";

    // Resume releases every carrier; the counter moves again.
    controller.resume();
    EXPECT_TRUE(spinUntil([&] { return g_counter.load() > c2; }))
        << "counter did not resume after continue";

    g_quit.store(true);
    __cajeta_task_shutdown();
    __cajeta_dbg_set_safepoint_handler(nullptr);
    g_ctrl = nullptr;
    __cajeta_stop_reset();
}

// §5.4: a carrier stuck in a native call can't reach a safepoint. The barrier
// must return within its bound, flagging the un-quiesced carrier — never hang.
TEST(DbgCarrierQuiesce, BoundedBarrierFlagsNativeStuckCarrier) {
    __cajeta_stop_reset();
    g_counter.store(0);
    g_quit.store(false);
    g_stuckRunning.store(false);
    setenv("CAJETA_CARRIERS", "4", 1);

    cajeta::dbg::DebugController controller;
    controller.setQuiesceTimeout(std::chrono::milliseconds(150));
    g_ctrl = &controller;
    controller.arm(kArmedLoc);
    __cajeta_dbg_set_safepoint_handler(testTrampoline);

    // 4 carriers: 1 armed (primary, fires only once the stuck fiber is provably
    // mid-native), 2 plain (park as secondaries), 1 stuck in a native call
    // (never parks) → expected=3, only 2 converge → exactly 1 un-quiesced.
    void* slots[4] = {nullptr};
    __cajeta_task_run(nullptr, plainFiber, &slots[0]);
    __cajeta_task_run(nullptr, plainFiber, &slots[1]);
    __cajeta_task_run(nullptr, nativeStuckFiber, &slots[2]);
    __cajeta_task_run(nullptr, armedAfterStuckFiber, &slots[3]);

    auto t0 = std::chrono::steady_clock::now();
    cajeta::dbg::StopEvent ev;
    ASSERT_TRUE(controller.waitForStop(ev, std::chrono::seconds(5)));
    auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(ev.locId, kArmedLoc);
    EXPECT_EQ(ev.unquiescedCarriers, 1)            // the native-stuck carrier
        << "barrier should flag exactly the un-quiesced carrier";
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500))   // honored the bound
        << "barrier hung waiting on the native-stuck carrier";

    controller.resume();
    g_quit.store(true);
    __cajeta_task_shutdown();
    __cajeta_dbg_set_safepoint_handler(nullptr);
    g_ctrl = nullptr;
    __cajeta_stop_reset();
}

// §5.5 regression: a single-carrier program stops and resumes exactly as
// before — the quiesce barrier is a no-op (expected = carriers − primary = 0),
// no deadlock, nothing flagged.
TEST(DbgCarrierQuiesce, SingleCarrierStopsAndResumesAsBefore) {
    __cajeta_stop_reset();
    g_counter.store(0);
    g_quit.store(false);
    setenv("CAJETA_CARRIERS", "1", 1);

    cajeta::dbg::DebugController controller;
    g_ctrl = &controller;
    controller.arm(kArmedLoc);
    __cajeta_dbg_set_safepoint_handler(testTrampoline);

    void* slot = nullptr;
    __cajeta_task_run(nullptr, armedFiber, &slot);

    cajeta::dbg::StopEvent ev;
    ASSERT_TRUE(controller.waitForStop(ev, std::chrono::seconds(5)));
    EXPECT_EQ(ev.locId, kArmedLoc);
    EXPECT_EQ(ev.unquiescedCarriers, 0);   // nothing else to quiesce

    controller.resume();
    EXPECT_TRUE(spinUntil([&] { return g_counter.load() > 0; }))
        << "single carrier did not resume";

    g_quit.store(true);
    __cajeta_task_shutdown();
    __cajeta_dbg_set_safepoint_handler(nullptr);
    g_ctrl = nullptr;
    __cajeta_stop_reset();
}

// §3.6: an armed exception quiesces the WHOLE pool before inspection — every
// other fiber AND a non-carrier entry/main thread running safepoints must
// freeze, not just the throwing carrier.
TEST(DbgCarrierQuiesce, ExceptionQuiescesPoolAndEntryThread) {
    __cajeta_stop_reset();
    g_counter.store(0);
    g_quit.store(false);
    setenv("CAJETA_CARRIERS", "3", 1);

    cajeta::dbg::DebugController controller;
    g_ctrl = &controller;
    controller.armException();
    __cajeta_dbg_set_safepoint_handler(testTrampoline);

    // A non-carrier "entry/main" thread running cajeta safepoints, like the
    // program's main. It must observe the stop and park too.
    std::atomic<long> entryCounter{0};
    std::thread entry([&] {
        while (!g_quit.load(std::memory_order_relaxed)) {
            __cajeta_dbg_safepoint(kPlainLoc);
            entryCounter.fetch_add(1, std::memory_order_relaxed);
        }
    });

    void* slots[3] = {nullptr};
    __cajeta_task_run(nullptr, plainFiber, &slots[0]);
    __cajeta_task_run(nullptr, plainFiber, &slots[1]);
    __cajeta_task_run(nullptr, throwingFiber, &slots[2]);

    cajeta::dbg::StopEvent ev;
    ASSERT_TRUE(controller.waitForStop(ev, std::chrono::seconds(5)));
    EXPECT_EQ(ev.reason, cajeta::dbg::StopEvent::StopReason::Exception);

    // Whole-pool freeze: neither the fibers' counter nor the entry thread moves.
    long f1 = g_counter.load(), e1 = entryCounter.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(f1, g_counter.load())   << "a fiber advanced during an exception stop";
    EXPECT_EQ(e1, entryCounter.load()) << "entry thread advanced during an exception stop";

    controller.resume();
    EXPECT_TRUE(spinUntil([&] {
        return g_counter.load() > f1 && entryCounter.load() > e1;
    })) << "pool/entry did not resume after the exception stop";

    g_quit.store(true);
    entry.join();
    __cajeta_task_shutdown();
    __cajeta_dbg_set_safepoint_handler(nullptr);
    g_ctrl = nullptr;
    __cajeta_stop_reset();
}

// §5.3: two carriers hit armed safepoints in the same round → exactly ONE
// primary stop; the other becomes a secondary (quiesced cleanly, not a second
// competing primary). Both resume together on continue.
TEST(DbgCarrierQuiesce, TwoArmedSafepointsOnePrimaryBothResume) {
    __cajeta_stop_reset();
    g_counter.store(0);
    g_quit.store(false);
    g_armedReady.store(0);
    setenv("CAJETA_CARRIERS", "2", 1);

    cajeta::dbg::DebugController controller;
    g_ctrl = &controller;
    controller.arm(42);
    controller.arm(43);
    __cajeta_dbg_set_safepoint_handler(testTrampoline);

    void* slots[2] = {nullptr};
    __cajeta_task_run(reinterpret_cast<void*>(42), armedOnceFiber, &slots[0]);
    __cajeta_task_run(reinterpret_cast<void*>(43), armedOnceFiber, &slots[1]);

    // Exactly one primary; the barrier converges (the OTHER armed carrier is a
    // clean secondary, not an uncounted second primary → unquiesced must be 0).
    cajeta::dbg::StopEvent ev;
    ASSERT_TRUE(controller.waitForStop(ev, std::chrono::seconds(5)));
    EXPECT_EQ(ev.reason, cajeta::dbg::StopEvent::StopReason::Breakpoint);
    EXPECT_TRUE(ev.locId == 42 || ev.locId == 43);
    EXPECT_EQ(ev.unquiescedCarriers, 0)
        << "second armed carrier did not quiesce as a secondary";

    // Frozen while stopped.
    long c1 = g_counter.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(c1, g_counter.load());

    // Resume-all: both carriers continue; no second primary stop appears (the
    // secondary already passed its armed loc), and the counter advances.
    controller.resume();
    cajeta::dbg::StopEvent ev2;
    EXPECT_FALSE(controller.waitForStop(ev2, std::chrono::milliseconds(300)))
        << "a spurious second stop appeared — both armed carriers were primary";
    long c2 = g_counter.load();
    EXPECT_TRUE(spinUntil([&] { return g_counter.load() > c2; }))
        << "carriers did not resume together";

    g_quit.store(true);
    __cajeta_task_shutdown();
    __cajeta_dbg_set_safepoint_handler(nullptr);
    g_ctrl = nullptr;
    __cajeta_stop_reset();
}
