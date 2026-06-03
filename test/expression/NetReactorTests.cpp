//
// NET-3.1 — Native reactor engine readiness intrinsics.
//
// The full reactor (epoll/kqueue/IOCP fiber-park + carrier-wake) is only
// meaningfully exercised against a live fiber scheduler; those end-to-end
// assertions are the JIT `ReactorTests.*` suite the plan names. What is
// deterministically pinnable at the native layer — and what this file pins —
// is the **portable readiness primitive** the reactor's await intrinsics are
// built on: `__cajeta_net_reactor_poll_fd`, plus the `init` / `register` /
// `deregister` ABI guarantees. These golden vectors run over a real loopback
// socket pair with no scheduler, the same discipline NetListenerTests /
// NetSocketTests use.
//
// The test binary links the C runtime natively and cajeta_runtime.c #includes
// cajeta_net_reactor.c (after the R9.4 reactor block + cajeta_net_socket.c,
// whose narrowing helpers it reuses), so these `__cajeta_net_*` symbols
// resolve via extern "C".
//
// Pins:
//   - poll_fd reports a fresh connected socket immediately writable   (freshSocketIsWritable)
//   - poll_fd parks on read until the peer sends, then fires          (readReadyAfterPeerSend)
//   - poll_fd times out (returns 0) when nothing is ready             (timeoutWhenIdle)
//   - poll_fd rejects a bad fd / empty interest with -1               (badFdAndEmptyInterestError)
//   - init is idempotent + returns success                           (initIdempotent)
//   - register/deregister honor the ABI (0 = ok, stable signature)   (registerDeregisterAbi)
//

#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

namespace {
    // Mirrors the native CAJETA_IO_READ / CAJETA_IO_WRITE + probe results.
    constexpr int32_t IO_READ  = 1;
    constexpr int32_t IO_WRITE = 2;
    constexpr int32_t R_READY    = 1;
    constexpr int32_t R_TIMEOUT  = 0;
    constexpr int32_t R_ERROR  = -1;
}

extern "C" {
    // NET-1.1 verbs reused to stand up a real loopback pair.
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_listen(int32_t fd, int32_t backlog);
    int32_t __cajeta_net_accept(int32_t fd, void* addr_out, int32_t* addrlen_inout);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len, int32_t flags);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_last_error(void);
    int32_t __cajeta_net_getsockname(int32_t fd, void* addr_out, int32_t* addrlen_inout);

    // NET-3.1 reactor ABI under test.
    int32_t __cajeta_net_reactor_init(void);
    int32_t __cajeta_net_reactor_register(int32_t fd, int32_t interest, void* fiber_handle);
    int32_t __cajeta_net_reactor_deregister(int32_t fd);
    int32_t __cajeta_net_reactor_poll_fd(int32_t fd, int32_t interest, int32_t timeout_ms);
    // The await intrinsics exist + link; on a non-fiber thread they fall
    // through to the portable probe, so we can smoke them too (off-fiber).
    int32_t __cajeta_net_await_writable(int32_t fd);
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

// Stand up a connected loopback TCP pair: returns the accepted server-side fd
// in *server_out and the client fd in *client_out. The listener is closed
// before returning. Returns true on success.
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
    // Discover the bound ephemeral port.
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
    // Blocking connect over loopback completes synchronously.
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

// --- init is idempotent + returns success ---------------------------------
// Lazy reactor init must be safe to call repeatedly (every await calls it) and
// must report success — on Windows it also ensures Winsock is up.
TEST(NetReactorTests, initIdempotent) {
    EXPECT_EQ(0, __cajeta_net_reactor_init());
    EXPECT_EQ(0, __cajeta_net_reactor_init());
    EXPECT_EQ(0, __cajeta_net_reactor_init());
}

// --- a fresh connected socket is immediately writable ---------------------
// A just-connected TCP socket has empty send buffers, so WRITE readiness fires
// at once. This is the connectAsync-completed signal NET-3.3 waits for.
TEST(NetReactorTests, freshSocketIsWritable) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli)) << "pair err=" << __cajeta_net_last_error();

    // A generous timeout; readiness should be effectively immediate.
    EXPECT_EQ(R_READY, __cajeta_net_reactor_poll_fd(cli, IO_WRITE, 1000));
    // And the await intrinsic (off-fiber → portable-probe fallback) agrees.
    EXPECT_EQ(R_READY, __cajeta_net_await_writable(cli));

    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}

// --- read readiness fires only after the peer sends -----------------------
// Before any data, a READ probe with a short timeout reports TIMEOUT (0): the
// socket is not readable. After the peer sends a byte, READ readiness fires.
// This is the readAsync park-then-wake shape, validated without a scheduler.
TEST(NetReactorTests, readReadyAfterPeerSend) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli)) << "pair err=" << __cajeta_net_last_error();

    // Nothing sent yet → not readable within a short window.
    EXPECT_EQ(R_TIMEOUT, __cajeta_net_reactor_poll_fd(srv, IO_READ, 100))
        << "server side was unexpectedly readable before any send";

    // Peer sends one byte.
    const char b = 'x';
    ASSERT_EQ(1, (int) __cajeta_net_send(cli, &b, 1, 0))
        << "send err=" << __cajeta_net_last_error();

    // Now readable (give the loopback stack a generous window).
    EXPECT_EQ(R_READY, __cajeta_net_reactor_poll_fd(srv, IO_READ, 1000))
        << "server side did not become readable after peer send";

    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}

// --- timeout path: idle fd returns TIMEOUT (0), not READY/ERROR -----------
// A read probe on an idle, healthy socket with a finite timeout must return
// exactly 0 — the value the NET-3.4 deadline composition keys on.
TEST(NetReactorTests, timeoutWhenIdle) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli));

    EXPECT_EQ(R_TIMEOUT, __cajeta_net_reactor_poll_fd(srv, IO_READ, 50));

    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}

// --- bad fd / empty interest are argument errors --------------------------
// A negative fd, and an interest mask with neither READ nor WRITE, both must
// return -1 (ERROR) rather than block or read as ready.
TEST(NetReactorTests, badFdAndEmptyInterestError) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    EXPECT_EQ(R_ERROR, __cajeta_net_reactor_poll_fd(-1, IO_READ, 10));

    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli));
    EXPECT_EQ(R_ERROR, __cajeta_net_reactor_poll_fd(srv, 0, 10))
        << "empty interest mask should be an argument error";
    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}

// --- register/deregister honor the v1 ABI ---------------------------------
// Under the one-shot model these are no-ops, but the ABI contract is that they
// accept the documented signature and return 0 (success) so the NET-3.3/3.4
// cancellation path can call deregister unconditionally.
TEST(NetReactorTests, registerDeregisterAbi) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli));

    EXPECT_EQ(0, __cajeta_net_reactor_register(srv, IO_READ, nullptr));
    EXPECT_EQ(0, __cajeta_net_reactor_deregister(srv));
    // Deregister of a never-registered fd is also a clean no-op (the
    // cancellation path may double-fire).
    EXPECT_EQ(0, __cajeta_net_reactor_deregister(cli));

    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}
