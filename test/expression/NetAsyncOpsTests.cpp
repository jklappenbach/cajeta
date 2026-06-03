//
// NET-3.3 — Async socket ops (connect / read / write / accept).
//
// The `cajeta.net` async forms (TcpStream.readAsync / writeAsync /
// writeAllAsync / connectAsync, TcpListener.acceptAsync, UdpSocket.recvFromAsync
// / sendToAsync) are pure-Cajeta **readiness loops** layered over the NET-1.7
// non-blocking primitives + the NET-3.1 reactor: each tries the non-blocking
// syscall; on WouldBlock it parks the fiber on the reactor (awaitReadable /
// awaitWritable) and retries. The end-to-end fiber-park assertions live in the
// JIT `ReactorTests.*` suite the plan names — they need the (not-yet-wired)
// compiler net-receiver dispatch AND a live carrier/scheduler, plus the
// kqueue/IOCP fiber engines that NET-3.1 explicitly defers off-Linux.
//
// What IS deterministically pinnable now — and what this file pins — is the
// **native readiness-loop the async bodies are built from**: drive the exact
// "non-blocking op → on WouldBlock, reactor await → retry" cycle over a real
// loopback pair with no scheduler, proving each async op's core advances
// correctly. This is the same scheduler-free golden-vector discipline
// NetReactorTests / NetNonBlockingTests use; the test binary links the C
// runtime natively (cajeta_runtime.c #includes cajeta_net_socket.c,
// cajeta_net_nonblocking.c, cajeta_net_reactor.c) so the symbols resolve via
// extern "C".
//
// Pins:
//   readAsyncLoopWakesOnPeerData   → recv WouldBlocks, reactor reports readable
//                                     after peer send, retry recv succeeds
//                                     (the TcpStream.readAsync loop core)
//   writeAsyncLoopWritableImmediate→ a fresh socket is writable, the writeAsync
//                                     loop sends without ever parking
//   acceptAsyncLoopWakesOnConnect  → accept WouldBlocks on an idle listener,
//                                     reactor reports readable on a pending
//                                     connect, retry accept succeeds
//                                     (the TcpListener.acceptAsync loop core)
//   recvFromAsyncLoopWakesOnDatagram→ UDP recvfrom WouldBlocks, reactor reports
//                                     readable after a datagram arrives, retry
//                                     succeeds (the UdpSocket.recvFromAsync loop)
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

extern "C" {
    // NET-1.1 verbs.
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_listen(int32_t fd, int32_t backlog);
    int32_t __cajeta_net_accept(int32_t fd, void* addr_out,
                                int32_t* addrlen_inout);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len,
                              int32_t flags);
    int64_t __cajeta_net_recv(int32_t fd, void* buf, int64_t len, int32_t flags);
    int64_t __cajeta_net_sendto(int32_t fd, const void* buf, int64_t len,
                                int32_t flags, const void* addr,
                                int32_t addrlen);
    int64_t __cajeta_net_recvfrom(int32_t fd, void* buf, int64_t len,
                                  int32_t flags, void* addr_out,
                                  int32_t* addrlen_inout);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_last_error(void);
    int32_t __cajeta_net_getsockname(int32_t fd, void* addr_out,
                                     int32_t* addrlen_inout);

    // NET-1.7 non-blocking primitives the async loops branch on.
    int32_t __cajeta_net_set_nonblocking(int32_t fd, int32_t nonblocking);
    int32_t __cajeta_net_is_wouldblock(void);

    // NET-3.1 reactor: the await body each async loop parks on.
    int32_t __cajeta_net_reactor_init(void);
    int32_t __cajeta_net_await_readable(int32_t fd);
    int32_t __cajeta_net_await_writable(int32_t fd);
}

namespace {

constexpr int32_t R_READY = 1;

sockaddr_in loopbackV4(uint16_t port) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

uint16_t boundPort(int32_t fd) {
    sockaddr_in a;
    int32_t len = (int32_t) sizeof(a);
    std::memset(&a, 0, sizeof(a));
    if (__cajeta_net_getsockname(fd, &a, &len) != 0) return 0;
    return ntohs(a.sin_port);
}

// Stand up a connected loopback TCP pair (server-accepted fd + client fd).
// The listener is closed before returning. Mirrors NetReactorTests' helper.
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
    uint16_t port = boundPort(lst);
    sockaddr_in dst = loopbackV4(port);
    int32_t cli = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    if (cli < 0) {
        __cajeta_net_close(lst);
        return false;
    }
    if (__cajeta_net_connect(cli, &dst, (int32_t) sizeof(dst)) != 0) {
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

// The native shape of the TcpStream.readAsync loop body: a single
// non-blocking recv attempt. Returns the byte count, or -1 on WouldBlock,
// or -2 on a hard error (so the test can assert the WouldBlock branch is
// taken exactly when no data is queued, the sentinel the cajeta loop keys on).
int64_t recvOrWouldBlock(int32_t fd, void* buf, int64_t len) {
    int64_t n = __cajeta_net_recv(fd, buf, len, 0);
    if (n >= 0) return n;
    if (__cajeta_net_is_wouldblock()) return -1;
    return -2;
}

}  // namespace

// --- readAsync loop: WouldBlock, then wake on peer data, then succeed ------
// Drives the exact TcpStream.readAsync cycle over loopback: the first
// non-blocking recv on an empty socket WouldBlocks; awaitReadable parks until
// the peer sends; the retried recv then returns the bytes. This is the loop the
// cajeta `readAsync` body runs, pinned without a scheduler.
TEST(NetAsyncOpsTests, readAsyncLoopWakesOnPeerData) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli))
        << "pair err=" << __cajeta_net_last_error();

    // The reader (server side) goes non-blocking — the precondition the async
    // form sets via setNonBlocking(true).
    ASSERT_EQ(0, __cajeta_net_set_nonblocking(srv, 1));

    char buf[16];

    // First attempt: nothing queued → the loop's WouldBlock branch.
    EXPECT_EQ(-1, recvOrWouldBlock(srv, buf, sizeof(buf)))
        << "empty non-blocking recv should classify as WouldBlock";

    // Peer sends; the reactor await (the loop's park point) then reports ready.
    const char payload[] = "ping";
    ASSERT_EQ(4, (int) __cajeta_net_send(cli, payload, 4, 0))
        << "send err=" << __cajeta_net_last_error();
    EXPECT_EQ(R_READY, __cajeta_net_await_readable(srv))
        << "reactor did not report the socket readable after peer send";

    // Retry: the recv now succeeds with the bytes — the loop's return path.
    std::memset(buf, 0, sizeof(buf));
    int64_t got = recvOrWouldBlock(srv, buf, sizeof(buf));
    EXPECT_EQ(4, (int) got);
    EXPECT_EQ(0, std::memcmp(buf, payload, 4));

    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}

// --- writeAsync loop: a fresh socket is writable, no park needed -----------
// A just-connected socket has empty send buffers, so awaitWritable fires at
// once and the loop's first send carries the data without ever parking — the
// common-case fast path of TcpStream.writeAsync.
TEST(NetAsyncOpsTests, writeAsyncLoopWritableImmediate) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());
    int32_t srv = -1, cli = -1;
    ASSERT_TRUE(makeConnectedPair(&srv, &cli));

    ASSERT_EQ(0, __cajeta_net_set_nonblocking(cli, 1));

    // The loop checks writability first; a fresh socket is immediately ready.
    EXPECT_EQ(R_READY, __cajeta_net_await_writable(cli));

    const char payload[] = "pong";
    int64_t sent = __cajeta_net_send(cli, payload, 4, 0);
    EXPECT_EQ(4, (int) sent) << "send err=" << __cajeta_net_last_error();

    // And the peer receives exactly those bytes (the round-trip the loop drives).
    char buf[16];
    std::memset(buf, 0, sizeof(buf));
    EXPECT_EQ(R_READY, __cajeta_net_await_readable(srv));
    int64_t got = __cajeta_net_recv(srv, buf, sizeof(buf), 0);
    EXPECT_EQ(4, (int) got);
    EXPECT_EQ(0, std::memcmp(buf, payload, 4));

    __cajeta_net_close(srv);
    __cajeta_net_close(cli);
}

// --- acceptAsync loop: WouldBlock on idle, wake on connect, then accept ----
// Drives the TcpListener.acceptAsync cycle: a non-blocking accept on an idle
// listener WouldBlocks; awaitReadable parks until a client connects (a listener
// becomes readable when a connection is queued); the retried accept then yields
// the connection fd.
TEST(NetAsyncOpsTests, acceptAsyncLoopWakesOnConnect) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    int32_t lst = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(lst, 0);
    sockaddr_in addr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(lst, &addr, (int32_t) sizeof(addr)));
    ASSERT_EQ(0, __cajeta_net_listen(lst, 4));
    uint16_t port = boundPort(lst);
    ASSERT_NE(0, port);

    // Listener goes non-blocking — the acceptAsync precondition.
    ASSERT_EQ(0, __cajeta_net_set_nonblocking(lst, 1));

    // Idle: the first accept attempt WouldBlocks (the loop's park branch).
    int32_t c0 = __cajeta_net_accept(lst, nullptr, nullptr);
    EXPECT_LT(c0, 0);
    EXPECT_EQ(1, __cajeta_net_is_wouldblock())
        << "idle non-blocking accept should classify as WouldBlock";

    // A client connects; the reactor await (loop park point) reports readable.
    int32_t cli = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(cli, 0);
    sockaddr_in dst = loopbackV4(port);
    ASSERT_EQ(0, __cajeta_net_connect(cli, &dst, (int32_t) sizeof(dst)));
    EXPECT_EQ(R_READY, __cajeta_net_await_readable(lst))
        << "listener did not become readable after a client connected";

    // Retry: the accept now yields the connection (the loop's return path).
    int32_t conn = __cajeta_net_accept(lst, nullptr, nullptr);
    EXPECT_GE(conn, 0) << "accept err=" << __cajeta_net_last_error();

    if (conn >= 0) __cajeta_net_close(conn);
    __cajeta_net_close(cli);
    __cajeta_net_close(lst);
}

// --- recvFromAsync loop: WouldBlock on idle UDP, wake on datagram ----------
// Drives the UdpSocket.recvFromAsync cycle: a non-blocking recvfrom on an idle
// datagram socket WouldBlocks; awaitReadable parks until a datagram arrives;
// the retried recvfrom then returns the payload + sender.
TEST(NetAsyncOpsTests, recvFromAsyncLoopWakesOnDatagram) {
    ASSERT_EQ(0, __cajeta_net_reactor_init());

    // Receiver: a bound, non-blocking UDP socket.
    int32_t rx = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(rx, 0);
    sockaddr_in raddr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(rx, &raddr, (int32_t) sizeof(raddr)));
    uint16_t rport = boundPort(rx);
    ASSERT_NE(0, rport);
    ASSERT_EQ(0, __cajeta_net_set_nonblocking(rx, 1));

    char buf[32];
    sockaddr_in from;
    int32_t fromLen = (int32_t) sizeof(from);

    // Idle: the first recvfrom WouldBlocks (the loop's park branch).
    std::memset(&from, 0, sizeof(from));
    int64_t n0 = __cajeta_net_recvfrom(rx, buf, sizeof(buf), 0, &from, &fromLen);
    EXPECT_LT((int) n0, 0);
    EXPECT_EQ(1, __cajeta_net_is_wouldblock())
        << "idle non-blocking recvfrom should classify as WouldBlock";

    // Sender fires a datagram; the reactor await reports the socket readable.
    int32_t tx = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(tx, 0);
    sockaddr_in dst = loopbackV4(rport);
    const char dg[] = "datagram";
    ASSERT_EQ(8, (int) __cajeta_net_sendto(tx, dg, 8, 0, &dst,
                                           (int32_t) sizeof(dst)))
        << "sendto err=" << __cajeta_net_last_error();
    EXPECT_EQ(R_READY, __cajeta_net_await_readable(rx))
        << "UDP socket did not become readable after a datagram arrived";

    // Retry: the recvfrom now returns the payload (the loop's return path).
    std::memset(buf, 0, sizeof(buf));
    fromLen = (int32_t) sizeof(from);
    std::memset(&from, 0, sizeof(from));
    int64_t got = __cajeta_net_recvfrom(rx, buf, sizeof(buf), 0, &from, &fromLen);
    EXPECT_EQ(8, (int) got);
    EXPECT_EQ(0, std::memcmp(buf, dg, 8));

    __cajeta_net_close(tx);
    __cajeta_net_close(rx);
}
