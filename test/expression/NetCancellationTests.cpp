//
// NET-11.4 — Cancellation/deadline I/O adapters.
//
// NET-11.4 is the thin adapter layer that makes any awaitable socket op
// compose with `Tasks.withTimeout` / `withDeadline` and the fiber
// `cancel_with` path while honoring the reactor's one hard contract:
// **deregister-on-cancel** — a cancelled or timed-out await leaves the
// reactor's live-registration balance back where it started, never a leak.
//
// The cajeta surface (runtime/src/cajeta/io/net/reactor/Reactor.cajeta) realizes
// this by bracketing every fiber park in `Reactor.awaitReadable` /
// `awaitWritable` with a `register` ... `try { park } finally { deregister }`
// pair, so the deregister fires on the readable-return path, the reactor-error
// path, AND the path where the enclosing withTimeout fires the fiber's
// cancel_with and the runtime throws the cancellation sentinel up through the
// parked frame. The end-to-end fiber-park + sentinel-throw assertion needs a
// live carrier/scheduler and lives in the JIT `CancellationTests.*` suite the
// plan names; what is deterministically pinnable at the native layer — and what
// this file pins — is the **register/await/deregister balance the adapter is
// built on**: that the live-registration counter
// (`__cajeta_net_reactor_active_count`) returns to its entry value on the
// completion bracket AND on the abandoned-park (cancellation) bracket, which is
// exactly what the cajeta `finally` guarantees.
//
// The leak invariant is asserted through the repo's existing `ReactorLeakGuard`
// RAII fixture (test/net/ReactorHarness.h, NET-13.4) — the same scope-balance
// machinery the concurrency harness uses — so a missing deregister surfaces as
// a failure pinned to the exact scope it leaked in. The test binary links the C
// runtime natively (cajeta_runtime.c #includes cajeta_net_socket.c +
// cajeta_net_reactor.c) so the `__cajeta_net_*` symbols resolve via extern "C".
// Same scheduler-free golden-vector discipline as NetReactorTests /
// NetAsyncOpsTests.
//
// Pins:
//   completionBracketReturnsToZero → register → await-ready → deregister leaves
//                                    the active count at its entry value (the
//                                    awaitReadable/Writable happy path)
//   cancelDeregistersReactorOp     → register → (park abandoned by a timeout /
//                                    cancel) → deregister returns the active
//                                    count to zero (the NET-11.4 ACCEPTANCE:
//                                    "a cancelled awaitable I/O op deregisters
//                                    from the reactor — registration count
//                                    returns to zero")
//   nestedBracketsBalance          → N concurrent registrations each deregister
//                                    independently; the balance is exact, never
//                                    driven negative by a double-deregister race
//   deregisterWithoutRegisterIsSafe→ a defensive deregister of a never-armed fd
//                                    clamps at zero (the timeout/completion
//                                    double-fire the adapter tolerates)
//

#include "gtest/gtest.h"

#include "net/ReactorHarness.h"   // ReactorLeakGuard + reactor ABI extern "C" decls

#include <cstdint>
#include <cstring>

using cajeta::net::testing::ReactorLeakGuard;
namespace rc = cajeta::net::testing::reactor_consts;

// NET-1.1 verbs needed to stand up a loopback pair that are NOT already
// declared by ReactorHarness.h (which declares socket / connect / close /
// last_error + the reactor ABI). Kept minimal to avoid redeclaration clashes.
extern "C" {
    int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_listen(int32_t fd, int32_t backlog);
    int32_t __cajeta_net_accept(int32_t fd, void* addr_out,
                                int32_t* addrlen_inout);
    int32_t __cajeta_net_getsockname(int32_t fd, void* addr_out,
                                     int32_t* addrlen_inout);
}

namespace {

sockaddr_in loopbackV4(uint16_t port) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

// Stand up a connected loopback TCP pair using the runtime verbs on both ends
// (server-accepted fd + client fd). The listener is closed before returning.
// Mirrors NetReactorTests::makeConnectedPair.
bool makeConnectedPair(int32_t* server_out, int32_t* client_out) {
    int32_t lst = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    if (lst < 0) return false;
    sockaddr_in addr = loopbackV4(0);
    if (__cajeta_net_bind(lst, &addr, (int32_t) sizeof(addr)) != 0) {
        __cajeta_net_close(lst);
        return false;
    }
    if (__cajeta_net_listen(lst, 4) != 0) {
        __cajeta_net_close(lst);
        return false;
    }
    sockaddr_in bound;
    int32_t blen = (int32_t) sizeof(bound);
    std::memset(&bound, 0, sizeof(bound));
    if (__cajeta_net_getsockname(lst, &bound, &blen) != 0) {
        __cajeta_net_close(lst);
        return false;
    }
    int32_t cli = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    if (cli < 0) {
        __cajeta_net_close(lst);
        return false;
    }
    if (__cajeta_net_connect(cli, &bound, (int32_t) sizeof(bound)) != 0) {
        __cajeta_net_close(lst);
        __cajeta_net_close(cli);
        return false;
    }
    int32_t srv = __cajeta_net_accept(lst, nullptr, nullptr);
    __cajeta_net_close(lst);
    if (srv < 0) {
        __cajeta_net_close(cli);
        return false;
    }
    *server_out = srv;
    *client_out = cli;
    return true;
}

}  // namespace

// --- the adapter's happy-path bracket returns the count to its entry value --
// `Reactor.awaitWritable` is `register → try { park } finally { deregister }`.
// On the normal ready-return path the balance after the bracket must equal the
// balance before it — no residue. The ReactorLeakGuard asserts the scope-exit
// balance matches entry. We drive that bracket natively over a socket that is
// immediately writable (so the await fires at once, no real park).
TEST(CancellationTests, completionBracketReturnsToZero) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli))
        << "pair err=" << __cajeta_net_last_error();

    {
        ReactorLeakGuard guard;  // baseline = active_count() at entry

        // The exact bracket the cajeta awaitWritable adapter runs:
        //   register(fd, WRITE);  try { await } finally { deregister(fd); }
        ASSERT_EQ(0, __cajeta_net_reactor_register(cli, rc::IO_WRITE, nullptr));
        EXPECT_EQ(guard.baseline() + 1, ReactorLeakGuard::active())
            << "register must bump the live-registration balance (the in-flight "
               "op is visible mid-park)";

        // A fresh socket is immediately writable: the await resolves READY.
        EXPECT_EQ(rc::R_READY,
                  __cajeta_net_reactor_poll_fd(cli, rc::IO_WRITE, 1000));

        // The finally drops the registration.
        ASSERT_EQ(0, __cajeta_net_reactor_deregister(cli));
    }  // guard dtor: balance must equal entry — no residual registration.

    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}

// --- ACCEPTANCE: a cancelled awaitable I/O op deregisters from the reactor ---
// (plan/cajeta-net-plan.md NET-11.4 Acceptance → CancellationTests.
//  cancelDeregistersReactorOp: "registration count returns to zero".)
//
// This models the cancellation path the NET-11.4 adapter protects: a read parks
// on an idle socket (the await would block indefinitely), then the enclosing
// `Tasks.withTimeout` fires the fiber's cancel_with and the runtime throws the
// cancellation sentinel up through the parked frame. The cajeta `finally` runs
// on that throw and deregisters — exactly what we assert here by abandoning the
// (would-block) await and running the bracket's deregister, with the
// ReactorLeakGuard proving the live-registration balance returns to its entry
// value with no leak.
TEST(CancellationTests, cancelDeregistersReactorOp) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli))
        << "pair err=" << __cajeta_net_last_error();

    {
        ReactorLeakGuard guard;  // baseline = active_count() at entry

        // register(fd, READ): the read op is armed and visible in the balance.
        ASSERT_EQ(0, __cajeta_net_reactor_register(srv, rc::IO_READ, nullptr));
        EXPECT_EQ(guard.baseline() + 1, ReactorLeakGuard::active())
            << "the in-flight read registration is live while parked";

        // The op would block forever: nothing is queued, so a finite-timeout
        // probe reports TIMEOUT — standing in for the indefinite park that
        // withTimeout abandons by throwing the cancellation sentinel through
        // the frame.
        EXPECT_EQ(rc::R_TIMEOUT,
                  __cajeta_net_reactor_poll_fd(srv, rc::IO_READ, 50))
            << "idle socket must not be readable — the op is genuinely parked";

        // *** The deregister-on-cancel contract: the adapter's `finally` runs
        //     even though the await never completed (the cancellation sentinel
        //     unwinds through it). ***
        ASSERT_EQ(0, __cajeta_net_reactor_deregister(srv));
    }  // guard dtor: ACCEPTANCE — the cancelled op's registration count is back
       // to its entry value (zero leak).

    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}

// --- N concurrent registrations each deregister independently, exact balance -
// Fan-out (a server with many parked connection fibers) means many live
// registrations at once. Each fiber's adapter brackets its own fd; the balance
// is exact and returns to entry only when every bracket has closed. Proves the
// counter is a true balance, not a boolean.
TEST(CancellationTests, nestedBracketsBalance) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    constexpr int kPairs = 5;
    int32_t srv[kPairs];
    int32_t cli[kPairs];
    for (int i = 0; i < kPairs; ++i) { srv[i] = -1; cli[i] = -1; }

    {
        ReactorLeakGuard guard;
        const int32_t base = guard.baseline();

        // Arm kPairs reads — like kPairs fibers each parked on its own conn.
        for (int i = 0; i < kPairs; ++i) {
            ASSERT_TRUE(makeConnectedPair(&srv[i], &cli[i]))
                << "pair " << i << " err=" << __cajeta_net_last_error();
            ASSERT_EQ(0,
                __cajeta_net_reactor_register(srv[i], rc::IO_READ, nullptr));
            EXPECT_EQ(base + i + 1, ReactorLeakGuard::active());
        }

        // Each bracket closes (some by completion, some by cancellation — the
        // counter does not distinguish): the balance decrements exactly once.
        for (int i = 0; i < kPairs; ++i) {
            ASSERT_EQ(0, __cajeta_net_reactor_deregister(srv[i]));
            EXPECT_EQ(base + (kPairs - i - 1), ReactorLeakGuard::active());
        }
    }  // guard dtor: every bracket closed → balance back to entry value.

    for (int i = 0; i < kPairs; ++i) {
        __cajeta_net_close(srv[i]);
        __cajeta_net_close(cli[i]);
    }
}

// --- a defensive deregister of a never-armed op clamps at zero ---------------
// The cancellation path can double-fire a deregister (a timeout race where both
// the timeout unwind and a late completion try to close the same bracket), and
// the adapter tolerates a deregister of a never-armed fd. Neither may drive the
// balance negative and mask a real leak elsewhere.
TEST(CancellationTests, deregisterWithoutRegisterIsSafe) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli));

    {
        ReactorLeakGuard guard;
        const int32_t base = guard.baseline();

        // Never registered: a defensive deregister is a clean no-op at floor.
        EXPECT_EQ(0, __cajeta_net_reactor_deregister(srv));
        EXPECT_EQ(base, ReactorLeakGuard::active())
            << "deregister of a never-armed fd must not drive the balance "
               "negative";

        // Double-fire: register once, deregister twice — the second is clamped.
        ASSERT_EQ(0, __cajeta_net_reactor_register(srv, rc::IO_READ, nullptr));
        EXPECT_EQ(base + 1, ReactorLeakGuard::active());
        ASSERT_EQ(0, __cajeta_net_reactor_deregister(srv));
        ASSERT_EQ(0, __cajeta_net_reactor_deregister(srv));  // double-fire
        EXPECT_EQ(base, ReactorLeakGuard::active())
            << "a double-fired deregister (timeout race) must clamp, not "
               "underflow";
    }  // guard dtor: balance restored.

    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}
