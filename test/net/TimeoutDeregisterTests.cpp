//
// TimeoutDeregisterTests.cpp — NET-3.4 timeout / cancellation integration.
//
// NET-3.4 is the integration that makes a deadline-bounded async socket op
// (a) *time out* when the fd stays idle past its budget and (b) leave the
// reactor's live-registration balance exactly where it started — the
// *deregister-on-timeout* half of the *deregister-on-cancel* contract every
// reactor op honors (see `docs/Net.md` §The async model and the plan's
// NET-3 acceptance `timedReadDeregistersOnTimeout`).
//
// The cajeta surface that lands with this item —
// `Reactor.awaitReadableTimed` / `awaitWritableTimed` (deadline-aware await
// bracketed by a register → poll-with-timeout → `finally`-deregister) and
// `TcpStream.readWithin` / `writeAllWithin` (single-call deadline that raises
// `TimedOutException` on expiry) — composes three already-tested native
// primitives:
//
//   __cajeta_net_reactor_register(fd, interest, h)  (NET-3.1)
//   __cajeta_net_reactor_poll_fd(fd, interest, ms)  (NET-3.1 portable probe)
//   __cajeta_net_reactor_deregister(fd)             (NET-3.1)
//   __cajeta_net_reactor_active_count()             (NET-3.1 leak signal)
//
// So the deterministic, scheduler-free half of NET-3.4 — *"a bounded wait on an
// idle fd returns TIMEOUT, and the bracket returns the registration balance to
// its entry value"* — is pinnable at the native ABI today over a real loopback
// socket, exactly the posture the NET-13.4 ReactorHarness takes for the untimed
// path. The full in-scheduler assertion (a fiber's `readWithin` raising
// `TimedOutException` while a sibling keeps running) is the JIT `ReactorTests.*`
// suite the plan names; this is its native analog and proves the registration
// arithmetic the cajeta `*Timed` adapters depend on.
//
// Kept under test/ so production sources carry no test-only surface.
//

#include "gtest/gtest.h"

#include "net/LoopbackFixtures.h"   // cross-platform socket shims + WSAStartup

#include <chrono>
#include <cstdint>

// The NET-3.1 reactor ABI the NET-3.4 deadline adapters compose, plus the
// NET-1.1 socket verbs used to stand up a connected loopback pair. extern "C"
// (the C runtime is linked natively into the test binary) — the same binding
// ReactorHarness.h / NetReactorTests use.
extern "C" {
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_close(int32_t fd);

    int32_t __cajeta_net_reactor_init(void);
    int32_t __cajeta_net_reactor_register(int32_t fd, int32_t interest, void* h);
    int32_t __cajeta_net_reactor_deregister(int32_t fd);
    int32_t __cajeta_net_reactor_active_count(void);
    int32_t __cajeta_net_reactor_poll_fd(int32_t fd, int32_t interest,
                                         int32_t timeout_ms);
}

namespace {

// Local mirror of the native readiness vocabulary + probe results. R_-prefixed
// because <windows.h> (via LoopbackFixtures.h) #defines a bare `ERROR`.
constexpr int32_t IO_READ   = 1;
constexpr int32_t IO_WRITE  = 2;
constexpr int32_t R_READY   = 1;
constexpr int32_t R_TIMEOUT = 0;
constexpr int32_t R_ERROR   = -1;

// Stand up one connected loopback pair. `*cli` is the *runtime* client fd the
// reactor verbs accept; `*peer` is the raw server-side socket the test drives
// the wake `::send` from. (Same split ReactorHarness::makePair uses — the two
// handle spaces are never mixed.) Returns true on success.
bool makePair(int32_t* cli, cajeta_net_test_socket_t* peer) {
    using namespace cajeta::net::testing;  // ensures Winsock is up on Windows
    cajeta_net_test_socket_t lst = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lst == CAJETA_NET_TEST_BAD_SOCKET) return false;
    int one = 1;
    ::setsockopt(lst, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(lst, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        CAJETA_NET_TEST_CLOSESOCK(lst); return false;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(lst, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        CAJETA_NET_TEST_CLOSESOCK(lst); return false;
    }
    if (::listen(lst, 4) < 0) { CAJETA_NET_TEST_CLOSESOCK(lst); return false; }

    int32_t c = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0) { CAJETA_NET_TEST_CLOSESOCK(lst); return false; }
    if (__cajeta_net_connect(c, &addr, (int32_t) sizeof(addr)) != 0) {
        __cajeta_net_close(c); CAJETA_NET_TEST_CLOSESOCK(lst); return false;
    }
    cajeta_net_test_socket_t p = ::accept(lst, nullptr, nullptr);
    CAJETA_NET_TEST_CLOSESOCK(lst);
    if (p == CAJETA_NET_TEST_BAD_SOCKET) { __cajeta_net_close(c); return false; }
    *cli = c;
    *peer = p;
    return true;
}

void closeAll(int32_t cli, cajeta_net_test_socket_t peer) {
    if (cli >= 0) __cajeta_net_close(cli);
    if (peer != CAJETA_NET_TEST_BAD_SOCKET) CAJETA_NET_TEST_CLOSESOCK(peer);
}

// The native body of `Reactor.awaitReadableTimed`: register interest, run the
// portable probe with a deadline, deregister on EVERY exit (here: the normal
// return). Returns the tri-state the cajeta adapter surfaces. Mirrors the
// register→poll→finally-deregister bracket exactly so this test exercises the
// real arithmetic the cajeta layer performs.
int32_t awaitReadableTimed(int32_t fd, int32_t interest, int32_t timeout_ms) {
    __cajeta_net_reactor_register(fd, interest, nullptr);
    int32_t r = R_ERROR;
    // No C++ exception can be thrown by the probe; the deregister always runs.
    r = __cajeta_net_reactor_poll_fd(fd, interest, timeout_ms);
    __cajeta_net_reactor_deregister(fd);
    return r;
}

} // namespace

// --- timedReadDeregistersOnTimeout ----------------------------------------
// The headline NET-3.4 acceptance: a deadline-bounded readable wait on an idle
// socket returns TIMEOUT, and the register→poll→deregister bracket leaves the
// live-registration balance back at its entry value (no leak on the timeout
// path — the same guarantee the cancellation path gives).
TEST(NetTimeoutDeregisterTests, timedReadDeregistersOnTimeout) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    int32_t cli = -1;
    cajeta_net_test_socket_t peer = CAJETA_NET_TEST_BAD_SOCKET;
    ASSERT_TRUE(makePair(&cli, &peer));

    const int32_t base = __cajeta_net_reactor_active_count();

    // The peer never sends, so the read side stays idle and the bounded wait
    // must elapse rather than report readable.
    auto t0 = std::chrono::steady_clock::now();
    int32_t r = awaitReadableTimed(cli, IO_READ, /*timeout_ms=*/40);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ(R_TIMEOUT, r)
        << "an idle socket's bounded readable wait must report TIMEOUT, not "
           "READY/ERROR";
    EXPECT_GE(elapsed, 30)
        << "the wait returned far too early to have honored its ~40ms deadline";
    EXPECT_EQ(base, __cajeta_net_reactor_active_count())
        << "the timeout path must deregister: the live-registration balance "
           "must return to its entry value (no reactor leak on timeout)";

    closeAll(cli, peer);
}

// --- timedReadReturnsReadyBeforeDeadline ----------------------------------
// The complement: when the peer sends inside the budget, the bounded wait
// reports READY (not TIMEOUT) and STILL deregisters cleanly — the deadline
// machinery does not perturb the registration balance on the success path.
TEST(NetTimeoutDeregisterTests, timedReadReturnsReadyBeforeDeadline) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    int32_t cli = -1;
    cajeta_net_test_socket_t peer = CAJETA_NET_TEST_BAD_SOCKET;
    ASSERT_TRUE(makePair(&cli, &peer));

    const int32_t base = __cajeta_net_reactor_active_count();

    // Data is already waiting, so a generous-deadline wait returns READY at once.
    const char byte = 'x';
    ASSERT_EQ(1, (int) ::send(peer, &byte, 1, 0));

    int32_t r = awaitReadableTimed(cli, IO_READ, /*timeout_ms=*/1000);
    EXPECT_EQ(R_READY, r)
        << "a socket with data ready must report READY before its deadline";
    EXPECT_EQ(base, __cajeta_net_reactor_active_count())
        << "the success path must also deregister (balance back to entry value)";

    closeAll(cli, peer);
}

// --- timedWriteReturnsReadyWhenWritable -----------------------------------
// The writable twin (the path `TcpStream.writeAllWithin` /
// `Reactor.awaitWritableTimed` drive): a fresh connected socket's send buffer
// has room, so a bounded writable wait reports READY immediately and the
// bracket leaves no registration behind.
TEST(NetTimeoutDeregisterTests, timedWriteReturnsReadyWhenWritable) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    int32_t cli = -1;
    cajeta_net_test_socket_t peer = CAJETA_NET_TEST_BAD_SOCKET;
    ASSERT_TRUE(makePair(&cli, &peer));

    const int32_t base = __cajeta_net_reactor_active_count();

    int32_t r = awaitReadableTimed(cli, IO_WRITE, /*timeout_ms=*/1000);
    EXPECT_EQ(R_READY, r)
        << "a freshly connected socket with send-buffer room must report "
           "writable READY before its deadline";
    EXPECT_EQ(base, __cajeta_net_reactor_active_count())
        << "the writable success path must deregister (no leak)";

    closeAll(cli, peer);
}

// --- repeatedTimeoutsDoNotAccumulateRegistrations -------------------------
// The leak invariant under repetition: many bounded waits that all time out
// must each deregister, so the balance never drifts upward across a wave — the
// accumulation a missing deregister-on-timeout would cause (and that would
// eventually exhaust the reactor). Mirrors the NET-13.4 "settled wave returns
// to zero" check, exercised on the *timeout* path specifically.
TEST(NetTimeoutDeregisterTests, repeatedTimeoutsDoNotAccumulateRegistrations) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    int32_t cli = -1;
    cajeta_net_test_socket_t peer = CAJETA_NET_TEST_BAD_SOCKET;
    ASSERT_TRUE(makePair(&cli, &peer));

    const int32_t base = __cajeta_net_reactor_active_count();

    for (int i = 0; i < 8; ++i) {
        int32_t r = awaitReadableTimed(cli, IO_READ, /*timeout_ms=*/5);
        EXPECT_EQ(R_TIMEOUT, r) << "idle-socket wait #" << i << " should time out";
        EXPECT_EQ(base, __cajeta_net_reactor_active_count())
            << "registration balance drifted after timed wait #" << i
            << " — a deregister-on-timeout was missed";
    }

    closeAll(cli, peer);
}
