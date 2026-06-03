//
// NET-1.3 — native socket-name query intrinsics (`getsockname`/`getpeername`).
//
// `TcpStream.localAddress()` / `peerAddress()` lower to these. The test binary
// links the C runtime natively (cajeta_runtime.c #includes
// cajeta_net_getname.c), so the `__cajeta_net_*` symbols resolve via extern "C"
// exactly as NetSocketTests / NetSockaddrTests do.
//
// These golden vectors pin the native layer NET-1.3 adds on top of NET-1.1.
// The full Cajeta-surface `TcpStream` connect/read/write/close round-trip is
// the compiler-codegen half of NET-1.3 (a `cajeta.net.TcpStream`-receiver
// dispatch block in MethodCallExpression.cpp, mirroring the File dispatch) and
// is pinned by the JIT suite once that codegen lands.
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
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_listen(int32_t fd, int32_t backlog);
    int32_t __cajeta_net_accept(int32_t fd, void* addr_out, int32_t* addrlen_inout);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_last_error(void);
    int32_t __cajeta_net_getsockname(int32_t fd, void* addr_out, int32_t* addrlen_inout);
    int32_t __cajeta_net_getpeername(int32_t fd, void* addr_out, int32_t* addrlen_inout);
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

}  // namespace

// --- getsockname reports the kernel-assigned local endpoint ----------------
//
// Bind a listener to 127.0.0.1:0 (ephemeral). getsockname must report the
// loopback address and a non-zero kernel-assigned port — this is exactly what
// TcpListener.localAddress() needs to surface the bound port back to the app.
TEST(NetGetNameTests, getSockNameReportsLocalEndpoint) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0) << "socket err=" << __cajeta_net_last_error();

    sockaddr_in addr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(fd, &addr, (int32_t) sizeof(addr)))
        << "bind err=" << __cajeta_net_last_error();

    sockaddr_in local;
    std::memset(&local, 0, sizeof(local));
    int32_t len = (int32_t) sizeof(local);
    ASSERT_EQ(0, __cajeta_net_getsockname(fd, &local, &len))
        << "getsockname err=" << __cajeta_net_last_error();

    ASSERT_GE(len, (int32_t) sizeof(sockaddr_in));
    EXPECT_EQ(local.sin_family, AF_INET);
    EXPECT_EQ(ntohl(local.sin_addr.s_addr), (uint32_t) INADDR_LOOPBACK);
    EXPECT_NE(ntohs(local.sin_port), 0) << "kernel must assign an ephemeral port";

    __cajeta_net_close(fd);
}

// --- getpeername reports the remote endpoint of a connected socket ---------
//
// Stand up a connected loopback pair, then getpeername on the client must
// report the server's listening port (its peer) and the loopback address —
// what TcpStream.peerAddress() returns.
TEST(NetGetNameTests, getPeerNameReportsRemoteEndpoint) {
    int32_t server = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server, 0);
    sockaddr_in saddr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(server, &saddr, (int32_t) sizeof(saddr)));
    ASSERT_EQ(0, __cajeta_net_listen(server, 16));

    // Discover the server's bound port via the intrinsic under test.
    sockaddr_in slocal;
    std::memset(&slocal, 0, sizeof(slocal));
    int32_t slen = (int32_t) sizeof(slocal);
    ASSERT_EQ(0, __cajeta_net_getsockname(server, &slocal, &slen));
    uint16_t serverPort = ntohs(slocal.sin_port);
    ASSERT_NE(serverPort, 0);

    int32_t client = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    sockaddr_in connectAddr = loopbackV4(serverPort);
    ASSERT_EQ(0, __cajeta_net_connect(client, &connectAddr, (int32_t) sizeof(connectAddr)))
        << "connect err=" << __cajeta_net_last_error();

    int32_t conn = __cajeta_net_accept(server, nullptr, nullptr);
    ASSERT_GE(conn, 0);

    // The client's peer is the server's listening endpoint.
    sockaddr_in peer;
    std::memset(&peer, 0, sizeof(peer));
    int32_t plen = (int32_t) sizeof(peer);
    ASSERT_EQ(0, __cajeta_net_getpeername(client, &peer, &plen))
        << "getpeername err=" << __cajeta_net_last_error();

    ASSERT_GE(plen, (int32_t) sizeof(sockaddr_in));
    EXPECT_EQ(peer.sin_family, AF_INET);
    EXPECT_EQ(ntohl(peer.sin_addr.s_addr), (uint32_t) INADDR_LOOPBACK);
    EXPECT_EQ(ntohs(peer.sin_port), serverPort);

    __cajeta_net_close(client);
    __cajeta_net_close(conn);
    __cajeta_net_close(server);
}

// --- getpeername on an unconnected socket fails (not connected) ------------
//
// An unconnected socket has no peer; the platform fails with ENOTCONN /
// WSAENOTCONN. The intrinsic must report -1 rather than fabricate an address,
// so TcpStream.peerAddress() on a not-yet-connected handle raises rather than
// returning garbage.
TEST(NetGetNameTests, getPeerNameOnUnconnectedFails) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    sockaddr_in peer;
    std::memset(&peer, 0, sizeof(peer));
    int32_t plen = (int32_t) sizeof(peer);
    EXPECT_EQ(-1, __cajeta_net_getpeername(fd, &peer, &plen))
        << "getpeername on an unconnected socket must fail";

    __cajeta_net_close(fd);
}

// --- argument guards: bad fd / null buffers / zero capacity ----------------
TEST(NetGetNameTests, argumentGuardsRejectBadInputs) {
    sockaddr_in s;
    int32_t len = (int32_t) sizeof(s);
    EXPECT_EQ(-1, __cajeta_net_getsockname(-1, &s, &len));
    EXPECT_EQ(-1, __cajeta_net_getpeername(-1, &s, &len));

    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(-1, __cajeta_net_getsockname(fd, nullptr, &len));
    EXPECT_EQ(-1, __cajeta_net_getsockname(fd, &s, nullptr));
    int32_t zero = 0;
    EXPECT_EQ(-1, __cajeta_net_getsockname(fd, &s, &zero));
    __cajeta_net_close(fd);
}
