//
// Tests for the DebugController stop/resume rendezvous (CP3, layer 1). Pure
// C++ + std::thread — no JIT — so these run in milliseconds and pin the
// breakpoint-park / continue handshake the runtime safepoint + DAP server
// build on.
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "cajeta/dbg/DebugController.h"

using cajeta::dbg::DebugController;
using cajeta::dbg::StopEvent;

TEST(DebugController, NotArmedReturnsImmediately) {
    DebugController c;
    // No arming: a safepoint must not block (would hang the test if it did).
    c.onSafepoint(5, 1);
    EXPECT_FALSE(c.isStopped());
}

TEST(DebugController, ArmingIsQueryable) {
    DebugController c;
    EXPECT_FALSE(c.isArmed(7));
    c.arm(7);
    EXPECT_TRUE(c.isArmed(7));
    c.disarm(7);
    EXPECT_FALSE(c.isArmed(7));
    c.arm(1);
    c.arm(2);
    c.clearArmed();
    EXPECT_FALSE(c.isArmed(1));
    EXPECT_FALSE(c.isArmed(2));
}

TEST(DebugController, ArmedSafepointParksThenResumes) {
    DebugController c;
    c.arm(7);

    std::atomic<bool> returned{false};
    std::thread carrier([&] {
        c.onSafepoint(7, 42);   // must block until resume()
        returned = true;
    });

    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.locId, 7);
    EXPECT_EQ(ev.fiberId, 42);
    // The carrier is parked inside onSafepoint (waitForStop only returns once
    // it parked), so it can't have set `returned` yet.
    EXPECT_FALSE(returned.load());
    EXPECT_TRUE(c.isStopped());

    c.resume();
    carrier.join();
    EXPECT_TRUE(returned.load());
    EXPECT_FALSE(c.isStopped());
}

// Regression: after a stop is observed and resumed, a subsequent waitForStop
// must NOT re-observe the same (now stale) stop. resume() clears `stopped`
// under the lock so the debugger thread doesn't see a phantom second stop.
// (This bug surfaced as a spurious extra `stopped` event on DAP `continue`.)
TEST(DebugController, ResumeClearsStaleStopForNextWait) {
    DebugController c;
    c.arm(7);

    std::thread carrier([&] { c.onSafepoint(7, 42); });
    (void) c.waitForStop();        // observe the stop
    c.resume();                    // release + clear stopped
    carrier.join();

    // No new safepoint will arm/park, so a bounded wait must time out (false),
    // not return the stale stop.
    StopEvent ev;
    EXPECT_FALSE(c.waitForStop(ev, std::chrono::milliseconds(100)));
    EXPECT_FALSE(c.isStopped());
}

// CP6f-3: onException no-ops unless exceptions are armed.
TEST(DebugController, ExceptionPassesThroughWhenNotArmed) {
    DebugController c;
    int dummy = 0;
    c.onException(&dummy, 1, nullptr);   // not armed -> must not block
    EXPECT_FALSE(c.isStopped());
    EXPECT_FALSE(c.isExceptionArmed());
}

TEST(DebugController, ExceptionArmingIsQueryable) {
    DebugController c;
    EXPECT_FALSE(c.isExceptionArmed());
    c.armException();
    EXPECT_TRUE(c.isExceptionArmed());
    c.disarmException();
    EXPECT_FALSE(c.isExceptionArmed());
}

// An armed exception parks like a breakpoint and carries reason=Exception plus
// the thrown value; resume() releases the throwing carrier.
TEST(DebugController, ArmedExceptionParksWithThrowableThenResumes) {
    DebugController c;
    c.armException();
    int thrown = 0;  // stand-in Throwable*; the controller is type-agnostic

    std::atomic<bool> returned{false};
    std::thread carrier([&] {
        c.onException(&thrown, 7, nullptr);   // must block until resume()
        returned = true;
    });

    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Exception);
    EXPECT_EQ(ev.throwable, &thrown);
    EXPECT_EQ(ev.fiberId, 7);
    EXPECT_EQ(ev.locId, -1);              // no safepoint loc for a throw
    EXPECT_FALSE(returned.load());
    EXPECT_TRUE(c.isStopped());

    c.resume();
    carrier.join();
    EXPECT_TRUE(returned.load());
    EXPECT_FALSE(c.isStopped());
}

TEST(DebugController, DisarmedLocPassesThrough) {
    DebugController c;
    c.arm(3);
    c.disarm(3);
    c.onSafepoint(3, 1);   // no longer armed -> must not block
    EXPECT_FALSE(c.isStopped());
}

// ---------------------------------------------------------------------------
// dap-stepping Unit 1: pending-step mode (spec 2.1, 2.2.1-2.2.6).
//
// The controller sees depth and line only through injected providers, so the
// fakes here encode both in the safepoint arguments: line = locId / 10 (locs
// 100 and 101 share line 10; loc 110 is line 11), and frameTop carries the
// depth as a pointer-sized integer.
// ---------------------------------------------------------------------------

using cajeta::dbg::StepKind;

namespace {
    void* D(int depth) {
        return reinterpret_cast<void*>(static_cast<intptr_t>(depth));
    }
    void installFakeProviders(DebugController& c) {
        c.setStepProviders(
            [](void* frameTop) {
                return static_cast<int>(reinterpret_cast<intptr_t>(frameTop));
            },
            [](int32_t locId) { return static_cast<int>(locId / 10); });
    }
}

// 1.1.1 Step-in parks at the first different-line safepoint on the origin
// fiber; a same-line safepoint before it passes through (multi-statement
// lines stop once).
TEST(DebugControllerStep, StepInParksAtNextDifferentLineSkippingSameLine) {
    DebugController c;
    installFakeProviders(c);
    c.arm(100);

    std::atomic<bool> done{false};
    std::thread carrier([&] {
        c.onSafepoint(100, 1, D(1));   // breakpoint park, line 10
        c.onSafepoint(101, 1, D(1));   // line 10 again — must not park
        c.onSafepoint(110, 1, D(1));   // line 11 — step park
        done = true;
    });

    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Breakpoint);
    c.resumeWithStep(StepKind::In, 1, 1, 10);

    ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Step);
    EXPECT_EQ(ev.locId, 110);          // 101 was skipped, or we'd have parked there
    EXPECT_FALSE(done.load());
    c.resume();
    carrier.join();
    EXPECT_TRUE(done.load());
}

// 1.1.2 Step-over ignores deeper safepoints (the stepped-over call's body)
// and parks at the first different-line safepoint at depth <= origin.
TEST(DebugControllerStep, StepOverSkipsDeeperFrames) {
    DebugController c;
    installFakeProviders(c);
    c.arm(100);

    std::thread carrier([&] {
        c.onSafepoint(100, 1, D(1));   // breakpoint park, line 10, depth 1
        c.onSafepoint(500, 1, D(2));   // callee body — deeper, no park
        c.onSafepoint(510, 1, D(3));   // nested callee — deeper, no park
        c.onSafepoint(110, 1, D(1));   // back at origin depth, line 11 — park
    });

    (void) c.waitForStop();
    c.resumeWithStep(StepKind::Over, 1, 1, 10);

    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Step);
    EXPECT_EQ(ev.locId, 110);
    c.resume();
    carrier.join();
}

// 1.1.3 Step-over off a method's last statement: the frame returns, so the
// next different-line safepoint is SHALLOWER — the <= rule parks there.
TEST(DebugControllerStep, StepOverOffLastStatementParksInCaller) {
    DebugController c;
    installFakeProviders(c);
    c.arm(100);

    std::thread carrier([&] {
        c.onSafepoint(100, 1, D(2));   // breakpoint on callee's last line
        c.onSafepoint(101, 1, D(2));   // same line — skip
        c.onSafepoint(300, 1, D(1));   // caller's next line, depth 1 < 2 — park
    });

    (void) c.waitForStop();
    c.resumeWithStep(StepKind::Over, 1, 2, 10);

    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Step);
    EXPECT_EQ(ev.locId, 300);
    c.resume();
    carrier.join();
}

// 1.1.4 Step-out: same-depth safepoints don't park; the first different-line
// safepoint at depth < origin does.
TEST(DebugControllerStep, StepOutSkipsSameDepthParksShallower) {
    DebugController c;
    installFakeProviders(c);
    c.arm(100);

    std::thread carrier([&] {
        c.onSafepoint(100, 1, D(2));   // breakpoint in callee, depth 2
        c.onSafepoint(110, 1, D(2));   // later line, SAME depth — no park
        c.onSafepoint(300, 1, D(1));   // caller, depth 1 — park
    });

    (void) c.waitForStop();
    c.resumeWithStep(StepKind::Out, 1, 2, 10);

    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Step);
    EXPECT_EQ(ev.locId, 300);
    c.resume();
    carrier.join();
}

// 1.1.5 Fiber isolation: another fiber's safepoints never satisfy a pending
// step — but an armed breakpoint on that fiber still parks (reason
// Breakpoint) and clears the step (spec 2.2.5).
TEST(DebugControllerStep, OtherFiberNeverSatisfiesStepButItsBreakpointWins) {
    DebugController c;
    installFakeProviders(c);
    c.arm(100);

    std::thread carrier1([&] { c.onSafepoint(100, 1, D(1)); });
    (void) c.waitForStop();
    c.resumeWithStep(StepKind::In, 1, 1, 10);
    carrier1.join();

    // Fiber 2, different line, would qualify on fiber 1 — must pass through.
    c.onSafepoint(300, 2, D(1));
    EXPECT_FALSE(c.isStopped());

    // Fiber 2 hits an armed breakpoint: parks as Breakpoint, step cleared.
    c.arm(400);
    std::thread carrier2([&] { c.onSafepoint(400, 2, D(1)); });
    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Breakpoint);
    EXPECT_EQ(ev.fiberId, 2);
    c.resume();
    carrier2.join();

    // The pending step is gone: a safepoint that WOULD have qualified on
    // fiber 1 now passes through.
    c.onSafepoint(110, 1, D(1));
    EXPECT_FALSE(c.isStopped());
}

// 1.1.6 An armed breakpoint on the origin fiber during a pending step parks
// with reason Breakpoint (breakpoint wins) and clears the step.
TEST(DebugControllerStep, BreakpointOnOriginFiberWinsOverPendingStep) {
    DebugController c;
    installFakeProviders(c);
    c.arm(100);
    c.arm(400);

    std::thread carrier1([&] { c.onSafepoint(100, 1, D(1)); });
    (void) c.waitForStop();
    c.resumeWithStep(StepKind::In, 1, 1, 10);
    carrier1.join();

    // Line 40 differs from origin line 10, so this qualifies as a step stop
    // too — the armed breakpoint must take precedence.
    std::thread carrier2([&] { c.onSafepoint(400, 1, D(5)); });
    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Breakpoint);
    EXPECT_EQ(ev.locId, 400);
    c.resume();
    carrier2.join();

    // Step cleared: a qualifying safepoint no longer parks.
    c.onSafepoint(110, 1, D(1));
    EXPECT_FALSE(c.isStopped());
}

// 1.1.7 A step that never finds a qualifying safepoint cannot wedge the
// controller: nothing parks for it, and a later armed breakpoint still
// stops normally (spec 2.2.6).
TEST(DebugControllerStep, UnsatisfiedStepNeverWedges) {
    DebugController c;
    installFakeProviders(c);
    c.arm(100);

    std::thread carrier1([&] { c.onSafepoint(100, 1, D(1)); });
    (void) c.waitForStop();
    // Step-out from depth 1: nothing shallower ever arrives.
    c.resumeWithStep(StepKind::Out, 1, 1, 10);
    carrier1.join();

    c.onSafepoint(110, 1, D(1));   // different line, same depth — no park
    c.onSafepoint(120, 1, D(2));   // deeper — no park
    EXPECT_FALSE(c.isStopped());

    // Breakpoints remain live while the step dangles.
    c.arm(200);
    std::thread carrier2([&] { c.onSafepoint(200, 1, D(3)); });
    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Breakpoint);
    EXPECT_EQ(ev.locId, 200);
    c.resume();
    carrier2.join();
}

// 1.1.8 Depth and line reach the controller only through the injected
// providers: the fakes record what they were asked about, and a controller
// with NO providers never parks for a step (safe default).
TEST(DebugControllerStep, ProvidersAreTheOnlySeamForDepthAndLine) {
    DebugController c;
    std::atomic<int32_t> lastLoc{-1};
    std::atomic<void*> lastFrame{nullptr};
    c.setStepProviders(
        [&](void* frameTop) {
            lastFrame = frameTop;
            return static_cast<int>(reinterpret_cast<intptr_t>(frameTop));
        },
        [&](int32_t locId) {
            lastLoc = locId;
            return static_cast<int>(locId / 10);
        });
    c.arm(100);

    std::thread carrier([&] {
        c.onSafepoint(100, 1, D(1));
        c.onSafepoint(110, 1, D(4));   // step park — providers consulted
    });
    (void) c.waitForStop();
    c.resumeWithStep(StepKind::In, 1, 1, 10);
    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.reason, StopEvent::StopReason::Step);
    EXPECT_EQ(lastLoc.load(), 110);
    EXPECT_EQ(lastFrame.load(), D(4));
    c.resume();
    carrier.join();

    // No providers -> a pending step can never match (and never blocks).
    DebugController bare;
    bare.arm(100);
    std::thread carrier2([&] { bare.onSafepoint(100, 1, D(1)); });
    (void) bare.waitForStop();
    bare.resumeWithStep(StepKind::In, 1, 1, 10);
    carrier2.join();
    bare.onSafepoint(110, 1, D(1));
    EXPECT_FALSE(bare.isStopped());
}

TEST(DebugController, OnlyArmedLocStops) {
    DebugController c;
    c.arm(10);
    // An un-armed loc passes straight through even while another is armed.
    c.onSafepoint(11, 1);
    EXPECT_FALSE(c.isStopped());

    std::atomic<bool> returned{false};
    std::thread carrier([&] {
        c.onSafepoint(10, 99);
        returned = true;
    });
    StopEvent ev = c.waitForStop();
    EXPECT_EQ(ev.locId, 10);
    EXPECT_EQ(ev.fiberId, 99);
    c.resume();
    carrier.join();
    EXPECT_TRUE(returned.load());
}
