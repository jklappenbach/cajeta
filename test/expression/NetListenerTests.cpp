//
// NET-1.4 — TcpListener native support intrinsics (bind / listen / accept,
// SO_REUSEADDR / SO_REUSEPORT, getsockname for localAddress).
//
// The verbs socket / bind / listen / accept / close are NET-1.1 and are
// pinned by NetSocketTests; NET-1.4 adds the listener-specific helpers that
// keep a platform constant (SOL_SOCKET / SO_REUSEADDR / SO_REUSEPORT) in C,
// plus getsockname for localAddress(). These golden vectors pin those at the
// native layer — the same discipline NetSocketTests / NetSockaddrTests use.
//
// The test binary links the C runtime natively and cajeta_runtime.c #includes
// cajeta_net_listener.c (after cajeta_net_socket.c, whose narrowing helpers it
// reuses), so these `__cajeta_net_*` symbols resolve via extern "C".
//
// Pins, at the listener level, the spec's NET-1.4 acceptance:
//   - bind + listen + accept round-trips a connection      (acceptViaListenerHelpers)
//   - SO_REUSEADDR sets + reads back                        (reuseAddrSetsAndReadsBack)
//   - SO_REUSEPORT is honored (real flag POSIX / no-op Win) (reusePortHonoredPerPlatform)
//   - getsockname reports the bound ephemeral port          (getsocknameReportsBoundPort)
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
    constexpr int32_t NET_OK = 0;
}

extern "C" {
    // NET-1.1 verbs reused here.
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_listen(int32_t fd, int32_t backlog);
    int32_t __cajeta_net_accept(int32_t fd, void* addr_out, int32_t* addrlen_inout);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len, int32_t flags);
    int64_t __cajeta_net_recv(int32_t fd, void* buf, int64_t len, int32_t flags);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_last_error(void);

    // NET-1.4 additions under test.
    int32_t __cajeta_net_set_reuseaddr(int32_t fd, int32_t enable);
    int32_t __cajeta_net_get_reuseaddr(int32_t fd);
    int32_t __cajeta_net_set_reuseport(int32_t fd, int32_t enable);
    int32_t __cajeta_net_has_reuseport(void);
    int32_t __cajeta_net_getsockname(int32_t fd, void* addr_out, int32_t* addrlen_inout);

    // NET-1.2 sizing helper, used to size the getsockname scratch buffer the
    // way the cajeta layer will.
    int32_t __cajeta_net_sockaddr_storage_size(void);
}

namespace {

// Build a loopback (127.0.0.1) sockaddr_in for `port` (host byte order).
sockaddr_in loopbackV4(uint16_t port) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

}  // namespace

// --- SO_REUSEADDR sets + reads back ---------------------------------------
// Pins the NET-1.4 slice of NetSocketTests.optionsSetAndGetBack: the
// listener's setReuseAddress(true) → getReuseAddress() == true identity,
// through the dedicated reuseaddr intrinsics (which hold SOL_SOCKET in C).
TEST(NetListenerTests, reuseAddrSetsAndReadsBack) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0) << "socket err=" << __cajeta_net_last_error();

    // Off initially on a fresh socket (its default is platform-dependent but
    // never spuriously on for SO_REUSEADDR; assert the round-trip, not the
    // initial value).
    ASSERT_EQ(0, __cajeta_net_set_reuseaddr(fd, 1))
        << "set_reuseaddr err=" << __cajeta_net_last_error();
    EXPECT_EQ(1, __cajeta_net_get_reuseaddr(fd)) << "REUSEADDR did not read back on";

    ASSERT_EQ(0, __cajeta_net_set_reuseaddr(fd, 0));
    EXPECT_EQ(0, __cajeta_net_get_reuseaddr(fd)) << "REUSEADDR did not read back off";

    __cajeta_net_close(fd);
}

// --- SO_REUSEADDR before bind lets bind succeed ---------------------------
// The functional point of REUSEADDR for a server: set it, then bind. The set
// must not error and the bind must succeed.
TEST(NetListenerTests, reuseAddrSetBeforeBindSucceeds) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, __cajeta_net_set_reuseaddr(fd, 1));

    sockaddr_in addr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(fd, &addr, (int32_t) sizeof(addr)))
        << "bind err=" << __cajeta_net_last_error();
    ASSERT_EQ(0, __cajeta_net_listen(fd, 16));

    __cajeta_net_close(fd);
}

// --- SO_REUSEPORT is honored per platform ---------------------------------
// On POSIX with a real SO_REUSEPORT the set succeeds (0). On Windows there is
// no such flag and the intrinsic is a definitional no-op that also returns 0.
// Either way the cajeta setReusePort(true) call never errors — that is the
// contract NET-1.4 ships.
TEST(NetListenerTests, reusePortHonoredPerPlatform) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    EXPECT_EQ(0, __cajeta_net_set_reuseport(fd, 1))
        << "set_reuseport(1) err=" << __cajeta_net_last_error();
    EXPECT_EQ(0, __cajeta_net_set_reuseport(fd, 0));

    // The has-flag probe agrees with the platform: 1 on POSIX, 0 on Windows.
#if defined(_WIN32)
    EXPECT_EQ(0, __cajeta_net_has_reuseport());
#else
    EXPECT_EQ(1, __cajeta_net_has_reuseport());
#endif

    __cajeta_net_close(fd);
}

// --- getsockname reports the bound ephemeral port -------------------------
// bind(...,port=0) lets the kernel pick a port; getsockname must report a
// non-zero loopback address + that chosen port — the exact data
// TcpListener.localAddress() returns after an ephemeral bind. Pins the
// localAddress() path.
TEST(NetListenerTests, getsocknameReportsBoundPort) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, __cajeta_net_set_reuseaddr(fd, 1));

    sockaddr_in addr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(fd, &addr, (int32_t) sizeof(addr)));
    ASSERT_EQ(0, __cajeta_net_listen(fd, 16));

    // Size the scratch buffer the way the cajeta layer will.
    int32_t cap = __cajeta_net_sockaddr_storage_size();
    ASSERT_GE(cap, (int32_t) sizeof(sockaddr_in));
    unsigned char scratch[256];
    ASSERT_LE(cap, (int32_t) sizeof(scratch));
    std::memset(scratch, 0, sizeof(scratch));
    int32_t len = cap;

    ASSERT_EQ(0, __cajeta_net_getsockname(fd, scratch, &len))
        << "getsockname err=" << __cajeta_net_last_error();
    ASSERT_GE(len, (int32_t) sizeof(sockaddr_in));

    sockaddr_in got;
    std::memcpy(&got, scratch, sizeof(got));
    EXPECT_EQ(got.sin_family, AF_INET);
    EXPECT_EQ(ntohl(got.sin_addr.s_addr), (uint32_t) INADDR_LOOPBACK);
    EXPECT_NE(ntohs(got.sin_port), 0) << "ephemeral bind did not yield a port";

    __cajeta_net_close(fd);
}

// --- getsockname rejects a bad / closed fd --------------------------------
TEST(NetListenerTests, getsocknameRejectsBadFd) {
    int32_t len = __cajeta_net_sockaddr_storage_size();
    unsigned char scratch[256];
    EXPECT_EQ(-1, __cajeta_net_getsockname(-1, scratch, &len));
    // NULL out / zero length are argument errors too.
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(-1, __cajeta_net_getsockname(fd, nullptr, &len));
    int32_t zero = 0;
    EXPECT_EQ(-1, __cajeta_net_getsockname(fd, scratch, &zero));
    __cajeta_net_close(fd);
}

// --- full bind → listen → accept round-trip via the listener helpers ------
// This is the listener-level mirror of NetSocketTests.tcpLoopbackEchoRoundTrips
// driven through the NET-1.4 surface the cajeta TcpListener.bind/accept lowers
// to: set_reuseaddr → bind → listen → getsockname(port) → accept; a client
// connects and the accepted fd round-trips a byte. Pins the
// "client connects, server accepts" half of the NET-1.4 acceptance.
TEST(NetListenerTests, acceptViaListenerHelpers) {
    // ---- server: socket → reuseaddr → bind(:0) → listen → getsockname ----
    int32_t server = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server, 0);
    ASSERT_EQ(0, __cajeta_net_set_reuseaddr(server, 1));
    sockaddr_in addr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(server, &addr, (int32_t) sizeof(addr)))
        << "bind err=" << __cajeta_net_last_error();
    ASSERT_EQ(0, __cajeta_net_listen(server, 16));

    int32_t len = __cajeta_net_sockaddr_storage_size();
    unsigned char scratch[256];
    std::memset(scratch, 0, sizeof(scratch));
    ASSERT_EQ(0, __cajeta_net_getsockname(server, scratch, &len));
    sockaddr_in bound;
    std::memcpy(&bound, scratch, sizeof(bound));
    uint16_t port = ntohs(bound.sin_port);
    ASSERT_NE(port, 0);

    // ---- client connects ----
    int32_t client = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    sockaddr_in connectAddr = loopbackV4(port);
    ASSERT_EQ(0, __cajeta_net_connect(client, &connectAddr, (int32_t) sizeof(connectAddr)))
        << "connect err=" << __cajeta_net_last_error();

    // ---- accept reports the peer address (the data accept() feeds the
    //      returned TcpStream.peerAddress()) ----
    int32_t peerLen = __cajeta_net_sockaddr_storage_size();
    unsigned char peerScratch[256];
    std::memset(peerScratch, 0, sizeof(peerScratch));
    int32_t conn = __cajeta_net_accept(server, peerScratch, &peerLen);
    ASSERT_GE(conn, 0) << "accept err=" << __cajeta_net_last_error();
    ASSERT_GE(peerLen, (int32_t) sizeof(sockaddr_in));
    sockaddr_in peer;
    std::memcpy(&peer, peerScratch, sizeof(peer));
    EXPECT_EQ(peer.sin_family, AF_INET);
    EXPECT_EQ(ntohl(peer.sin_addr.s_addr), (uint32_t) INADDR_LOOPBACK);

    // ---- one byte round-trips over the accepted connection ----
    const char ping[] = "L";
    ASSERT_EQ((int64_t) sizeof(ping),
              __cajeta_net_send(client, ping, (int64_t) sizeof(ping), 0));
    char buf[16];
    int64_t got = __cajeta_net_recv(conn, buf, (int64_t) sizeof(buf), 0);
    ASSERT_EQ(got, (int64_t) sizeof(ping));
    EXPECT_EQ(0, std::memcmp(ping, buf, sizeof(ping)));

    EXPECT_EQ(0, __cajeta_net_close(client));
    EXPECT_EQ(0, __cajeta_net_close(conn));
    EXPECT_EQ(0, __cajeta_net_close(server));
}
