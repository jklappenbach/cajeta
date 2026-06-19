//
// NET-1.1 — native socket intrinsics (BSD sockets / Winsock).
//
// The test binary links the C runtime natively (src/CMakeLists.txt adds
// CAJETA_RT_SRC to the test target as well as compiling it to bitcode for the
// JIT), and cajeta_runtime.c #includes cajeta_net_socket.c, so these
// `__cajeta_net_*` symbols are present and callable directly via extern "C".
//
// This exercises ONLY the native layer that NET-1.1 ships — the `cajeta.io.net`
// Cajeta surface (IpAddress / SocketAddress / TcpStream / sockaddr
// marshalling) is NET-1.2+ and not yet built. So the test constructs
// `sockaddr_in` directly (the job NET-1.2's `__cajeta_net_sockaddr_pack`
// will own) and drives the intrinsics over IPv4 loopback. These are the
// behavioral golden vectors the plan's `NetSocketTests.*` acceptance
// checkboxes pin at the native level:
//
//   - tcpLoopbackEchoRoundTrips        → blocking TCP echo over loopback
//   - connectRefusedCitesAddress       → connect to a closed port → REFUSED
//   - udpLoopbackRecvFromReportsPeer    → UDP datagram + sender address
//   - optionsSetAndGetBack             → SO_REUSEADDR set + read back
//   - nonblockingRecvReturnsWouldBlock  → non-blocking recv → WOULDBLOCK
//
// The higher-level "client connects, server accepts + echoes" Cajeta-surface
// versions of these land with NET-1.3 / NET-1.4 and reuse the same vectors.
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
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#endif

// --- The NET-1.1 cross-platform error ordinals (mirror cajeta_net_socket.c) ---
namespace {
    constexpr int32_t NET_OK = 0;
    constexpr int32_t NET_WOULDBLOCK = 1;
    constexpr int32_t NET_CONNECTION_REFUSED = 2;
}

extern "C" {
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_listen(int32_t fd, int32_t backlog);
    int32_t __cajeta_net_accept(int32_t fd, void* addr_out, int32_t* addrlen_inout);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len, int32_t flags);
    int64_t __cajeta_net_recv(int32_t fd, void* buf, int64_t len, int32_t flags);
    int64_t __cajeta_net_sendto(int32_t fd, const void* buf, int64_t len,
                                int32_t flags, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_recvfrom(int32_t fd, void* buf, int64_t len, int32_t flags,
                                  void* addr_out, int32_t* addrlen_inout);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_shutdown(int32_t fd, int32_t how);
    int32_t __cajeta_net_setsockopt(int32_t fd, int32_t level, int32_t optname,
                                    const void* optval, int32_t optlen);
    int32_t __cajeta_net_getsockopt(int32_t fd, int32_t level, int32_t optname,
                                    void* optval, int32_t* optlen_inout);
    int32_t __cajeta_net_set_nonblocking(int32_t fd, int32_t nonblocking);
    int32_t __cajeta_net_last_error(void);
}

namespace {

// Build a loopback (127.0.0.1) sockaddr_in for `port` (host byte order).
// `port == 0` asks the kernel to pick an ephemeral port at bind time.
sockaddr_in loopbackV4(uint16_t port) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

// Read back the kernel-assigned address (after a bind(...,port=0)) so the
// client knows the ephemeral port to connect to. Uses getsockname directly —
// that's a query helper NET-1.3 (localAddress) will wrap; here we call the
// platform fn since the intrinsic for it is NET-1.3.
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

}  // namespace

// --- TCP loopback echo round-trip -----------------------------------------
//
// Server: socket → bind(127.0.0.1:0) → listen → accept.
// Client: socket → connect → send. Server echoes; client reads it back.
// Pins NetSocketTests.tcpLoopbackEchoRoundTrips at the native layer.
TEST(NetSocketTests, tcpLoopbackEchoRoundTrips) {
    int32_t server = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server, 0) << "socket() failed, err=" << __cajeta_net_last_error();

    sockaddr_in addr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(server, &addr, (int32_t) sizeof(addr)))
        << "bind err=" << __cajeta_net_last_error();
    ASSERT_EQ(0, __cajeta_net_listen(server, 16));

    uint16_t port = boundPort(server);
    ASSERT_NE(port, 0);

    int32_t client = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    sockaddr_in connectAddr = loopbackV4(port);
    ASSERT_EQ(0, __cajeta_net_connect(client, &connectAddr, (int32_t) sizeof(connectAddr)))
        << "connect err=" << __cajeta_net_last_error();

    int32_t conn = __cajeta_net_accept(server, nullptr, nullptr);
    ASSERT_GE(conn, 0) << "accept err=" << __cajeta_net_last_error();

    const char msg[] = "cajeta-net-ping";
    int64_t sent = __cajeta_net_send(client, msg, (int64_t) sizeof(msg), 0);
    ASSERT_EQ(sent, (int64_t) sizeof(msg));

    // Server reads, echoes back.
    char inbuf[64];
    int64_t got = __cajeta_net_recv(conn, inbuf, (int64_t) sizeof(inbuf), 0);
    ASSERT_EQ(got, (int64_t) sizeof(msg));
    int64_t echoed = __cajeta_net_send(conn, inbuf, got, 0);
    ASSERT_EQ(echoed, got);

    // Client reads the echo back.
    char back[64];
    int64_t backN = __cajeta_net_recv(client, back, (int64_t) sizeof(back), 0);
    ASSERT_EQ(backN, (int64_t) sizeof(msg));
    EXPECT_EQ(0, std::memcmp(msg, back, sizeof(msg)));

    EXPECT_EQ(0, __cajeta_net_shutdown(client, 2));
    EXPECT_EQ(0, __cajeta_net_close(client));
    EXPECT_EQ(0, __cajeta_net_close(conn));
    EXPECT_EQ(0, __cajeta_net_close(server));
}

// --- connect to a closed port → ConnectionRefused -------------------------
//
// Bind+listen, capture the port, then close the listener so the port is dead,
// and connect to it. The normalized errno must be CONNECTION_REFUSED.
// Pins NetSocketTests.connectRefusedCitesAddress (the "cites address" part is
// the Cajeta wrapper's job in NET-1.8; here we pin the errno mapping).
TEST(NetSocketTests, connectRefusedMapsToRefusedErrno) {
    int32_t probe = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(probe, 0);
    sockaddr_in addr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(probe, &addr, (int32_t) sizeof(addr)));
    ASSERT_EQ(0, __cajeta_net_listen(probe, 1));
    uint16_t deadPort = boundPort(probe);
    ASSERT_NE(deadPort, 0);
    // Close the listener: nothing is now listening on deadPort.
    ASSERT_EQ(0, __cajeta_net_close(probe));

    int32_t client = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    sockaddr_in connectAddr = loopbackV4(deadPort);
    int32_t r = __cajeta_net_connect(client, &connectAddr, (int32_t) sizeof(connectAddr));
    ASSERT_EQ(r, -1) << "connect to a closed port unexpectedly succeeded";
    EXPECT_EQ(NET_CONNECTION_REFUSED, __cajeta_net_last_error());

    __cajeta_net_close(client);
}

// --- UDP datagram round-trip + recvfrom reports the sender ----------------
// Pins NetSocketTests.udpLoopbackRecvFromReportsPeer.
TEST(NetSocketTests, udpLoopbackRecvFromReportsPeer) {
    int32_t receiver = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(receiver, 0);
    sockaddr_in rAddr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(receiver, &rAddr, (int32_t) sizeof(rAddr)));
    uint16_t rPort = boundPort(receiver);
    ASSERT_NE(rPort, 0);

    int32_t sender = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(sender, 0);
    // Bind the sender too so it has a known source port the receiver can report.
    sockaddr_in sBind = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(sender, &sBind, (int32_t) sizeof(sBind)));
    uint16_t sPort = boundPort(sender);

    const char dgram[] = "udp-datagram";
    sockaddr_in dst = loopbackV4(rPort);
    int64_t sent = __cajeta_net_sendto(sender, dgram, (int64_t) sizeof(dgram), 0,
                                       &dst, (int32_t) sizeof(dst));
    ASSERT_EQ(sent, (int64_t) sizeof(dgram));

    char buf[64];
    sockaddr_in from;
    std::memset(&from, 0, sizeof(from));
    int32_t fromLen = (int32_t) sizeof(from);
    int64_t got = __cajeta_net_recvfrom(receiver, buf, (int64_t) sizeof(buf), 0,
                                        &from, &fromLen);
    ASSERT_EQ(got, (int64_t) sizeof(dgram));
    EXPECT_EQ(0, std::memcmp(dgram, buf, sizeof(dgram)));

    // recvfrom must have reported the sender's loopback address + port.
    ASSERT_GE(fromLen, (int32_t) sizeof(sockaddr_in));
    EXPECT_EQ(from.sin_family, AF_INET);
    EXPECT_EQ(ntohl(from.sin_addr.s_addr), (uint32_t) INADDR_LOOPBACK);
    EXPECT_EQ(ntohs(from.sin_port), sPort);

    __cajeta_net_close(sender);
    __cajeta_net_close(receiver);
}

// --- socket option set + read back ----------------------------------------
// Pins NetSocketTests.optionsSetAndGetBack (SO_REUSEADDR slice).
TEST(NetSocketTests, optionsSetAndGetBack) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    int32_t on = 1;
    ASSERT_EQ(0, __cajeta_net_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                                         &on, (int32_t) sizeof(on)))
        << "setsockopt err=" << __cajeta_net_last_error();

    int32_t readBack = 0;
    int32_t len = (int32_t) sizeof(readBack);
    ASSERT_EQ(0, __cajeta_net_getsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                                         &readBack, &len));
    EXPECT_NE(readBack, 0) << "SO_REUSEADDR did not read back as enabled";

    __cajeta_net_close(fd);
}

// --- non-blocking recv on an empty socket returns WouldBlock --------------
// Pins NetSocketTests.nonblockingRecvReturnsWouldBlock — WouldBlock is a
// distinct normalized errno, NOT a hard error, so the reactor (Phase 3) can
// drive readiness loops on it.
TEST(NetSocketTests, nonblockingRecvReturnsWouldBlock) {
    // Stand up a connected loopback pair so recv has a real, empty socket.
    int32_t server = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server, 0);
    sockaddr_in addr = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(server, &addr, (int32_t) sizeof(addr)));
    ASSERT_EQ(0, __cajeta_net_listen(server, 1));
    uint16_t port = boundPort(server);

    int32_t client = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    sockaddr_in connectAddr = loopbackV4(port);
    ASSERT_EQ(0, __cajeta_net_connect(client, &connectAddr, (int32_t) sizeof(connectAddr)));
    int32_t conn = __cajeta_net_accept(server, nullptr, nullptr);
    ASSERT_GE(conn, 0);

    // Put the client in non-blocking mode; nothing has been sent to it, so a
    // recv must report WouldBlock rather than blocking or erroring hard.
    ASSERT_EQ(0, __cajeta_net_set_nonblocking(client, 1));

    char buf[16];
    int64_t got = __cajeta_net_recv(client, buf, (int64_t) sizeof(buf), 0);
    ASSERT_EQ(got, -1) << "non-blocking recv on an empty socket should not return data";
    EXPECT_EQ(NET_WOULDBLOCK, __cajeta_net_last_error());

    // Sanity: after data arrives, the same non-blocking recv succeeds.
    const char ping[] = "x";
    ASSERT_EQ((int64_t) sizeof(ping),
              __cajeta_net_send(conn, ping, (int64_t) sizeof(ping), 0));
    // Brief readiness spin (loopback delivery is near-instant but not
    // synchronous); bounded so a real failure still terminates.
    int64_t after = -1;
    for (int i = 0; i < 1000; ++i) {
        after = __cajeta_net_recv(client, buf, (int64_t) sizeof(buf), 0);
        if (after >= 0) break;
        ASSERT_EQ(NET_WOULDBLOCK, __cajeta_net_last_error());
    }
    EXPECT_EQ(after, (int64_t) sizeof(ping));

    __cajeta_net_close(client);
    __cajeta_net_close(conn);
    __cajeta_net_close(server);
}

// --- close is idempotent at the -1 sentinel -------------------------------
// The cajeta layer nulls its fd field to -1 after close; calling the
// intrinsic again with -1 must be a no-op success (matches File.close()).
TEST(NetSocketTests, closeOnNegativeFdIsNoOpSuccess) {
    EXPECT_EQ(0, __cajeta_net_close(-1));
}
