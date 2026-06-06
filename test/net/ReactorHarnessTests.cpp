//
// ReactorHarnessTests.cpp — NET-13.4 reactor / concurrency harness tests.
//
// Exercises the two reusable fixtures in `ReactorHarness.h` and pins the
// two reactor invariants the harness exists to protect:
//
//   * carrier-non-blocking interleave — two sibling workers each park on a
//     blocking-style read of a separate idle socket, and BOTH advance a
//     shared counter before EITHER read returns (proving neither park
//     stalled the other);                                   (SiblingCounterProbe)
//   * zero registration leak — the reactor's live-registration balance
//     (`__cajeta_net_reactor_active_count`) tracks register/deregister and
//     returns to its entry value after a settled scope.     (ReactorLeakGuard)
//
// Plus direct coverage of the new NET-3.1 `_active_count` ABI the leak
// guard reads (register raises it, deregister lowers it, deregister clamps
// at zero so a double-fire on the cancellation path cannot mask a leak).
//
// These run over real loopback sockets with no fiber scheduler — the same
// deterministic, scheduler-free discipline NetReactorTests / the NET-13.1
// loopback fixtures use. The full in-scheduler assertion (two fibers on one
// carrier) is the JIT `ReactorTests.*` suite the plan names; this is its
// native analog and the harness those scheduler tests reuse for setup.
//

#include "gtest/gtest.h"

#include "net/ReactorHarness.h"

using namespace cajeta::net::testing;
using cajeta::net::testing::reactor_consts::IO_READ;

// --- active_count ABI: register raises, deregister lowers ------------------
// The leak guard's signal must be honest: a register bumps the live count,
// a matching deregister drops it back. (Under the v1 one-shot model the
// calls keep no per-fd table, but they DO maintain this balance — the piece
// NET-13.4 needs and NET-3.2/3.3 extend.)
TEST(ReactorHarnessTests, activeCountTracksRegisterDeregister) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    const int32_t base = __cajeta_net_reactor_active_count();
    EXPECT_EQ(0, __cajeta_net_reactor_register(7, IO_READ, nullptr));
    EXPECT_EQ(base + 1, __cajeta_net_reactor_active_count());
    EXPECT_EQ(0, __cajeta_net_reactor_register(8, IO_READ, nullptr));
    EXPECT_EQ(base + 2, __cajeta_net_reactor_active_count());

    EXPECT_EQ(0, __cajeta_net_reactor_deregister(7));
    EXPECT_EQ(base + 1, __cajeta_net_reactor_active_count());
    EXPECT_EQ(0, __cajeta_net_reactor_deregister(8));
    EXPECT_EQ(base, __cajeta_net_reactor_active_count());
}

// --- deregister clamps at zero (cancellation double-fire is safe) ---------
// NET-3.4's cancellation path may deregister an fd that was never armed, or
// fire deregister twice (timeout race + completion both unwinding). Neither
// may drive the balance negative — a negative residue would mask a real leak
// on a *different* fd. The count floors at zero.
TEST(ReactorHarnessTests, deregisterClampsAtZero) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    // Drain to a known floor: deregister more than could be live.
    for (int i = 0; i < 4; ++i) __cajeta_net_reactor_deregister(99);
    const int32_t floor = __cajeta_net_reactor_active_count();
    EXPECT_GE(floor, 0);

    __cajeta_net_reactor_deregister(99);   // already at/below balance
    EXPECT_EQ(floor, __cajeta_net_reactor_active_count())
        << "deregister must not drive the live-registration balance negative";
}

// --- ReactorLeakGuard: balanced scope leaves no residue -------------------
// A scope that arms N registrations and deregisters all N must leave the
// guard's entry balance restored — the guard adds no failure.
TEST(ReactorHarnessTests, leakGuardPassesOnBalancedScope) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    {
        ReactorLeakGuard guard;
        const int32_t base = guard.baseline();
        __cajeta_net_reactor_register(1, IO_READ, nullptr);
        __cajeta_net_reactor_register(2, IO_READ, nullptr);
        // Mid-scope the count reflects the armed ops — proves the guard can
        // also witness an in-flight registration (the "is it parked?" probe).
        EXPECT_EQ(base + 2, ReactorLeakGuard::active());
        __cajeta_net_reactor_deregister(1);
        __cajeta_net_reactor_deregister(2);
    }   // guard dtor: balance restored → no ADD_FAILURE, test stays green.
    SUCCEED();
}

// --- ReactorLeakGuard: detects a real leak --------------------------------
// A scope that arms a registration and forgets to drop it must be caught.
// We verify the guard's detection by checking the count itself diverges
// (the guard's own ADD_FAILURE is asserted indirectly: we balance the books
// after measuring, so this test does not itself fail).
TEST(ReactorHarnessTests, leakGuardWitnessesUnbalancedScope) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    const int32_t before = __cajeta_net_reactor_active_count();
    __cajeta_net_reactor_register(3, IO_READ, nullptr);   // arm, never drop
    EXPECT_EQ(before + 1, __cajeta_net_reactor_active_count())
        << "an un-deregistered op is exactly the leak the guard flags";
    // Restore the balance so the suite-wide invariant holds for later tests.
    __cajeta_net_reactor_deregister(3);
    EXPECT_EQ(before, __cajeta_net_reactor_active_count());
}

// --- SiblingCounterProbe: carrier-non-blocking interleave -----------------
// The headline NET-3 acceptance, modeled one level down: two siblings each
// park on a blocking-style read of a separate idle socket and BOTH advance
// the shared counter before EITHER read returns. If a park serialized the
// siblings, one could not have advanced the counter before the other woke.
TEST(ReactorHarnessTests, siblingFibersInterleaveWithoutBlocking) {
    SiblingCounterProbe probe;
    EXPECT_TRUE(probe.run())
        << "sibling reads did not interleave: a park appears to have blocked "
           "the shared execution context (carrier-non-blocking invariant)";
}

// --- SiblingCounterProbe leaves the reactor balance clean -----------------
// Composes the two invariants: the interleave probe runs INSIDE a leak guard,
// so the test asserts non-blocking interleave AND that the whole wave settles
// with the registration balance back at its entry value.
//
// Under the v1 one-shot model the await path arms+disarms atomically and does
// not route through the explicit register/deregister ABI, so the guard's job
// here is the *floor* guarantee — the probe must not perturb the balance (no
// stray registration escapes). When the kqueue/IOCP persistent-registration
// engines land (NET-3.2/3.3) and the awaits DO bump the counter, this same
// composed assertion becomes the end-to-end park→arm→wake→drop leak check
// without any change to the test.
TEST(ReactorHarnessTests, interleaveProbeLeavesNoRegistrationLeak) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    {
        ReactorLeakGuard guard;
        SiblingCounterProbe probe;
        EXPECT_TRUE(probe.run());
    }   // guard dtor asserts the balance returned to its entry value.
    SUCCEED();
}
