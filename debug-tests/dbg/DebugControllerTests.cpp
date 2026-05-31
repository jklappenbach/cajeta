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

TEST(DebugController, DisarmedLocPassesThrough) {
    DebugController c;
    c.arm(3);
    c.disarm(3);
    c.onSafepoint(3, 1);   // no longer armed -> must not block
    EXPECT_FALSE(c.isStopped());
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
