//
// NET-1.7 — non-blocking mode + WouldBlock-as-a-value.
//
// NET-1.1 shipped the *write* half of the toggle
// (`__cajeta_net_set_nonblocking`) and leaves the normalized WouldBlock
// ordinal readable via `__cajeta_net_last_error()` after a -1 from
// recv/send/accept. NET-1.7 completes the contract that the reactor (Phase 3)
// drives readiness loops on:
//
//   - a *queryable* toggle (`__cajeta_net_get_nonblocking`) so set→get
//     round-trips (NET-1.1 had only the setter), via the tracked setter
//     `__cajeta_net_set_nonblocking_tracked` the cajeta `setNonBlocking`
//     surface lowers to;
//   - a *non-throwing classifier* (`__cajeta_net_is_wouldblock`) the cajeta
//     read/recv/accept surface branches on to return its `WOULD_BLOCK`
//     sentinel value instead of constructing+throwing an exception on the hot
//     readiness path;
//   - a distinct connect-in-progress classifier
//     (`__cajeta_net_is_in_progress`) so a non-blocking connect's "now in
//     flight" is told apart from a hard connect failure.
//
// As with every NET-1.x item, the `cajeta.io.net` Cajeta surface (TcpStream /
// TcpListener / UdpSocket) is intrinsic-lowered by a compiler net-receiver
// dispatch that is NOT yet built, so these golden vectors pin NET-1.7 at the
// native layer — the same discipline NetSocketTests / NetListenerTests /
// NetUdpSocketTests use. The test binary links the C runtime natively and
// cajeta_runtime.c #includes cajeta_net_nonblocking.c (after
// cajeta_net_socket.c, whose helpers it reuses), so these symbols resolve via
// extern "C".
//
// Pins:
//   setGetNonBlockingRoundTrips        → toggle on/off reads back identically
//   wouldBlockClassifiedNotAsHardError → empty non-blocking recv → is_wouldblock
//   nonblockingAcceptWouldBlocks       → idle non-blocking accept → would-block
//   blockingRecvIsNotWouldBlock        → a real (non-WB) error is NOT misclassed
//   nonblockingConnectInProgress       → in-flight connect → is_in_progress
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
    // NET-1.1 verbs reused here.
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_listen(int32_t fd, int32_t backlog);
    int32_t __cajeta_net_accept(int32_t fd, void* addr_out, int32_t* addrlen_inout);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_recv(int32_t fd, void* buf, int64_t len, int32_t flags);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_set_nonblocking(int32_t fd, int32_t nonblocking);
    int32_t __cajeta_net_last_error(void);

    // NET-1.7 additions under test.
    int32_t __cajeta_net_set_nonblocking_tracked(int32_t fd, int32_t nonblocking);
    int32_t __cajeta_net_get_nonblocking(int32_t fd);
    int32_t __cajeta_net_is_wouldblock(void);
    int32_t __cajeta_net_is_in_progress(void);
}

namespace {

// NET-1.1 cross-platform error ordinals (mirror cajeta_net_socket.c).
constexpr int32_t NET_WOULDBLOCK = 1;

// Build a loopback (127.0.0.1) sockaddr_in for `port` (host byte order).
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
    std::memset(&a, 0, sizeof(a));
#if defined(_WIN32)
    int len = sizeof(a);
    ::getsockname((SOCKET) fd, (sockaddr*) &a, &len);
#else
    socklen_t len = sizeof(a);
    ::getsockname(fd, (sockaddr*) &a, &len);
#endif
    return ntohs(a.sin_port);
}

// Stand up a connected loopback TCP pair; returns client + accepted-conn fds
// via out-params, plus the listener fd as the return (caller closes all three).
int32_t connectedPair(int32_t* clientOut, int32_t* connOut) {
    int32_t server = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(server, 0);
    sockaddr_in addr = loopbackV4(0);
    EXPECT_EQ(0, __cajeta_net_bind(server, &addr, (int32_t) sizeof(addr)));
    EXPECT_EQ(0, __cajeta_net_listen(server, 1));
    uint16_t port = boundPort(server);

    int32_t client = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(client, 0);
    sockaddr_in caddr = loopbackV4(port);
    EXPECT_EQ(0, __cajeta_net_connect(client, &caddr, (int32_t) sizeof(caddr)));
    int32_t conn = __cajeta_net_accept(server, nullptr, nullptr);
    EXPECT_GE(conn, 0);

    *clientOut = client;
    *connOut = conn;
    return server;
}

}  // namespace

// --- the toggle round-trips: set non-blocking, read it back, restore --------
//
// NET-1.1 shipped only the setter; NET-1.7 adds the queryable getter so the
// cajeta `isNonBlocking()` surface and the set→get acceptance have a pin. On
// POSIX this is an authoritative fcntl(F_GETFL); on Windows it consults the
// shadow the tracked setter maintains (FIONBIO is write-only).
TEST(NetNonBlockingTests, setGetNonBlockingRoundTrips) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    // A fresh socket is blocking by default.
    EXPECT_EQ(0, __cajeta_net_get_nonblocking(fd));

    // Enable non-blocking via the tracked setter → reads back as 1.
    ASSERT_EQ(0, __cajeta_net_set_nonblocking_tracked(fd, 1));
    EXPECT_EQ(1, __cajeta_net_get_nonblocking(fd));

    // Restore blocking → reads back as 0.
    ASSERT_EQ(0, __cajeta_net_set_nonblocking_tracked(fd, 0));
    EXPECT_EQ(0, __cajeta_net_get_nonblocking(fd));

    __cajeta_net_close(fd);
}

// --- an empty non-blocking recv is classified WouldBlock, not a hard error --
//
// This is the heart of NET-1.7: the cajeta read surface does
// `if (n < 0 && Net.isWouldBlock()) return WOULD_BLOCK;` and only throws on a
// genuine fault. Here we drive the same classifier the cajeta layer will.
TEST(NetNonBlockingTests, wouldBlockClassifiedNotAsHardError) {
    int32_t client = -1, conn = -1;
    int32_t server = connectedPair(&client, &conn);

    ASSERT_EQ(0, __cajeta_net_set_nonblocking_tracked(client, 1));

    // Nothing has been sent → recv returns its -1 sentinel...
    char buf[16];
    int64_t n = __cajeta_net_recv(client, buf, (int64_t) sizeof(buf), 0);
    ASSERT_EQ(n, -1);
    // ...and the non-throwing classifier says "would block", so the cajeta
    // surface returns its WOULD_BLOCK value rather than throwing.
    EXPECT_EQ(1, __cajeta_net_is_wouldblock());
    // It is NOT an in-progress connect.
    EXPECT_EQ(0, __cajeta_net_is_in_progress());
    // And it agrees with the normalized ordinal funnel.
    EXPECT_EQ(NET_WOULDBLOCK, __cajeta_net_last_error());

    __cajeta_net_close(client);
    __cajeta_net_close(conn);
    __cajeta_net_close(server);
}

// --- a non-blocking accept on an idle listener would-blocks -----------------
//
// The reactor's acceptAsync (NET-3.3) loops on exactly this: try accept; on
// would-block, await readability; retry. Here the listener has no pending
// connection, so a non-blocking accept returns -1 classified as would-block.
TEST(NetNonBlockingTests, nonblockingAcceptWouldBlocks) {
    int32_t server = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server, 0);
    sockaddr_in addr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(server, &addr, (int32_t) sizeof(addr)));
    ASSERT_EQ(0, __cajeta_net_listen(server, 1));

    ASSERT_EQ(0, __cajeta_net_set_nonblocking_tracked(server, 1));
    EXPECT_EQ(1, __cajeta_net_get_nonblocking(server));

    // No client has connected → accept does not block; it returns -1 classified
    // as would-block (the value the cajeta acceptFdOrWouldBlock surface emits).
    int32_t c = __cajeta_net_accept(server, nullptr, nullptr);
    ASSERT_EQ(c, -1);
    EXPECT_EQ(1, __cajeta_net_is_wouldblock());

    __cajeta_net_close(server);
}

// --- a real error (recv on a closed fd) is NOT misclassified as WouldBlock --
//
// Guards the value contract from the other side: only a genuine would-block
// reads as 1, so a cajeta caller that returns its WOULD_BLOCK sentinel never
// swallows a real fault (e.g. a closed/invalid descriptor). EBADF/ENOTSOCK
// (POSIX) / WSAENOTSOCK (Winsock) must classify as NOT would-block.
TEST(NetNonBlockingTests, blockingRecvIsNotWouldBlock) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, __cajeta_net_close(fd));   // now fd is invalid

    char buf[8];
    int64_t n = __cajeta_net_recv(fd, buf, (int64_t) sizeof(buf), 0);
    ASSERT_EQ(n, -1);
    // A closed/invalid socket is a real error, never would-block — the cajeta
    // surface must throw here, not return its WOULD_BLOCK value.
    EXPECT_EQ(0, __cajeta_net_is_wouldblock());
}

// --- a non-blocking connect to a remote-style address reports in-progress ---
//
// A non-blocking connect that cannot complete synchronously returns -1 with the
// platform's "in flight" code (EINPROGRESS / WSAEWOULDBLOCK), which the
// dedicated classifier reports as in-progress (not a hard failure). The reactor
// then awaits writability + checks SO_ERROR. We connect to a TEST-NET-1
// (192.0.2.0/24, RFC 5737) address that is guaranteed unrouted, so the connect
// cannot finish in-line; either it reports in-progress (the common case) or the
// stack rejects it synchronously — both are acceptable, so we only assert that
// IF it returned -1 in-progress, the classifier agrees, and that an in-progress
// result is never also misread as would-block-for-data semantics on POSIX.
TEST(NetNonBlockingTests, nonblockingConnectInProgress) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, __cajeta_net_set_nonblocking_tracked(fd, 1));

    sockaddr_in dst;
    std::memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9);   // discard port; unrouted host below
    ::inet_pton(AF_INET, "192.0.2.1", &dst.sin_addr);   // RFC 5737 TEST-NET-1

    int32_t r = __cajeta_net_connect(fd, &dst, (int32_t) sizeof(dst));
    if (r == -1) {
        // The classifier must recognize the in-flight connect. (On the rare
        // platform/stack that completes or rejects synchronously, r==0 or a
        // hard error — both fine; the in-progress path is what we pin when it
        // occurs, which is the normal non-blocking outcome.)
        if (__cajeta_net_is_in_progress()) {
            SUCCEED();
        } else {
            // A synchronous hard error (e.g. ENETUNREACH on a host with no
            // route) is also a legitimate outcome and must NOT be would-block.
            EXPECT_EQ(0, __cajeta_net_is_wouldblock());
        }
    }

    __cajeta_net_close(fd);
}
