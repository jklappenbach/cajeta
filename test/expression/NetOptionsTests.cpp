//
// NET-1.6 — typed socket-option surface native intrinsics.
//
// The Cajeta `SocketOption`-keyed setters/getters on TcpStream / UdpSocket /
// TcpListener lower to dedicated `__cajeta_net_set_<opt>` / `_get_<opt>`
// intrinsics that hold every platform option constant (SOL_SOCKET /
// IPPROTO_TCP / IPPROTO_IP / IPPROTO_IPV6, TCP_NODELAY, SO_*, IP_TTL,
// IPV6_*) in C — exactly as NET-1.4 keeps SO_REUSEADDR behind
// __cajeta_net_set_reuseaddr. These golden vectors pin the set -> read-back
// identity (and the kernel-honored-intent cases) at the native layer, the
// same discipline NetListenerTests / NetSocketTests use.
//
// The test binary links the C runtime natively and cajeta_runtime.c #includes
// cajeta_net_socket_options.c (after cajeta_net_socket.c, whose narrowing
// helpers it reuses), so these `__cajeta_net_*` symbols resolve via extern "C".
//
// Pins the spec's NET-1.6 option list:
//   NO_DELAY, KEEP_ALIVE, BROADCAST, ONLY_V6  -> boolean set/get round-trip
//   RECV_BUFFER_SIZE / SEND_BUFFER_SIZE       -> set grows the effective size
//   LINGER                                    -> on/seconds set + read-back
//   TTL                                       -> v4 IP_TTL set/get round-trip
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
    // NET-1.1 verbs reused to make sockets under test.
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_last_error(void);

    // NET-1.6 typed option surface under test.
    int32_t __cajeta_net_set_nodelay(int32_t fd, int32_t on);
    int32_t __cajeta_net_get_nodelay(int32_t fd);
    int32_t __cajeta_net_set_keepalive(int32_t fd, int32_t on);
    int32_t __cajeta_net_get_keepalive(int32_t fd);
    int32_t __cajeta_net_set_recvbuf(int32_t fd, int32_t bytes);
    int32_t __cajeta_net_get_recvbuf(int32_t fd);
    int32_t __cajeta_net_set_sendbuf(int32_t fd, int32_t bytes);
    int32_t __cajeta_net_get_sendbuf(int32_t fd);
    int32_t __cajeta_net_set_linger(int32_t fd, int32_t on, int32_t seconds);
    int32_t __cajeta_net_get_linger(int32_t fd, int32_t* on_out, int32_t* seconds_out);
    int32_t __cajeta_net_set_broadcast(int32_t fd, int32_t on);
    int32_t __cajeta_net_get_broadcast(int32_t fd);
    int32_t __cajeta_net_set_ttl(int32_t fd, int32_t is_v6, int32_t ttl);
    int32_t __cajeta_net_get_ttl(int32_t fd, int32_t is_v6);
    int32_t __cajeta_net_set_only_v6(int32_t fd, int32_t on);
    int32_t __cajeta_net_get_only_v6(int32_t fd);
}

namespace {

int32_t makeTcp4() { return __cajeta_net_socket(AF_INET, SOCK_STREAM, 0); }
int32_t makeUdp4() { return __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0); }
int32_t makeTcp6() { return __cajeta_net_socket(AF_INET6, SOCK_STREAM, 0); }

}  // namespace

// --- TCP_NODELAY sets + reads back ----------------------------------------
TEST(NetOptionsTests, noDelaySetsAndReadsBack) {
    int32_t fd = makeTcp4();
    ASSERT_GE(fd, 0) << "socket err=" << __cajeta_net_last_error();

    ASSERT_EQ(0, __cajeta_net_set_nodelay(fd, 1))
        << "set_nodelay err=" << __cajeta_net_last_error();
    EXPECT_EQ(1, __cajeta_net_get_nodelay(fd)) << "TCP_NODELAY did not read back on";

    ASSERT_EQ(0, __cajeta_net_set_nodelay(fd, 0));
    EXPECT_EQ(0, __cajeta_net_get_nodelay(fd)) << "TCP_NODELAY did not read back off";

    __cajeta_net_close(fd);
}

// --- SO_KEEPALIVE sets + reads back ---------------------------------------
TEST(NetOptionsTests, keepAliveSetsAndReadsBack) {
    int32_t fd = makeTcp4();
    ASSERT_GE(fd, 0);

    ASSERT_EQ(0, __cajeta_net_set_keepalive(fd, 1))
        << "set_keepalive err=" << __cajeta_net_last_error();
    EXPECT_EQ(1, __cajeta_net_get_keepalive(fd));

    ASSERT_EQ(0, __cajeta_net_set_keepalive(fd, 0));
    EXPECT_EQ(0, __cajeta_net_get_keepalive(fd));

    __cajeta_net_close(fd);
}

// --- SO_RCVBUF / SO_SNDBUF: set grows the effective size ------------------
// The kernel may round/clamp/double the request, so we assert the *intent*
// was honored (the effective size grew toward the request), not byte equality.
TEST(NetOptionsTests, bufferSizesGrowOnSet) {
    int32_t fd = makeTcp4();
    ASSERT_GE(fd, 0);

    int32_t baseRecv = __cajeta_net_get_recvbuf(fd);
    ASSERT_GE(baseRecv, 0) << "get_recvbuf err=" << __cajeta_net_last_error();
    ASSERT_EQ(0, __cajeta_net_set_recvbuf(fd, 200000))
        << "set_recvbuf err=" << __cajeta_net_last_error();
    int32_t newRecv = __cajeta_net_get_recvbuf(fd);
    ASSERT_GE(newRecv, 0);
    EXPECT_GT(newRecv, baseRecv) << "SO_RCVBUF did not grow on set";

    int32_t baseSend = __cajeta_net_get_sendbuf(fd);
    ASSERT_GE(baseSend, 0);
    ASSERT_EQ(0, __cajeta_net_set_sendbuf(fd, 200000));
    int32_t newSend = __cajeta_net_get_sendbuf(fd);
    ASSERT_GE(newSend, 0);
    EXPECT_GT(newSend, baseSend) << "SO_SNDBUF did not grow on set";

    __cajeta_net_close(fd);
}

// --- a negative buffer-size request is an argument error ------------------
TEST(NetOptionsTests, negativeBufferSizeRejected) {
    int32_t fd = makeTcp4();
    ASSERT_GE(fd, 0);
    EXPECT_EQ(-1, __cajeta_net_set_recvbuf(fd, -1));
    EXPECT_EQ(-1, __cajeta_net_set_sendbuf(fd, -1));
    __cajeta_net_close(fd);
}

// --- SO_LINGER: on + seconds sets and reads back --------------------------
TEST(NetOptionsTests, lingerSetsAndReadsBack) {
    int32_t fd = makeTcp4();
    ASSERT_GE(fd, 0);

    ASSERT_EQ(0, __cajeta_net_set_linger(fd, 1, 7))
        << "set_linger err=" << __cajeta_net_last_error();
    int32_t on = -1, secs = -1;
    ASSERT_EQ(0, __cajeta_net_get_linger(fd, &on, &secs))
        << "get_linger err=" << __cajeta_net_last_error();
    EXPECT_EQ(1, on) << "SO_LINGER did not read back on";
    EXPECT_EQ(7, secs) << "SO_LINGER seconds did not round-trip";

    // Disabling linger zeroes the on flag (the seconds value is then moot).
    ASSERT_EQ(0, __cajeta_net_set_linger(fd, 0, 0));
    on = -1;
    ASSERT_EQ(0, __cajeta_net_get_linger(fd, &on, nullptr));
    EXPECT_EQ(0, on) << "SO_LINGER did not read back off";

    __cajeta_net_close(fd);
}

// --- SO_BROADCAST on a UDP socket sets + reads back -----------------------
TEST(NetOptionsTests, broadcastSetsAndReadsBackOnUdp) {
    int32_t fd = makeUdp4();
    ASSERT_GE(fd, 0) << "udp socket err=" << __cajeta_net_last_error();

    ASSERT_EQ(0, __cajeta_net_set_broadcast(fd, 1))
        << "set_broadcast err=" << __cajeta_net_last_error();
    EXPECT_EQ(1, __cajeta_net_get_broadcast(fd));

    ASSERT_EQ(0, __cajeta_net_set_broadcast(fd, 0));
    EXPECT_EQ(0, __cajeta_net_get_broadcast(fd));

    __cajeta_net_close(fd);
}

// --- IP_TTL (v4) sets + reads back ----------------------------------------
TEST(NetOptionsTests, ttlV4SetsAndReadsBack) {
    int32_t fd = makeUdp4();
    ASSERT_GE(fd, 0);

    ASSERT_EQ(0, __cajeta_net_set_ttl(fd, /*is_v6=*/0, 42))
        << "set_ttl err=" << __cajeta_net_last_error();
    EXPECT_EQ(42, __cajeta_net_get_ttl(fd, /*is_v6=*/0))
        << "IP_TTL did not round-trip";

    __cajeta_net_close(fd);
}

// --- an out-of-range TTL is an argument error -----------------------------
TEST(NetOptionsTests, ttlOutOfRangeRejected) {
    int32_t fd = makeUdp4();
    ASSERT_GE(fd, 0);
    EXPECT_EQ(-1, __cajeta_net_set_ttl(fd, 0, -1));
    EXPECT_EQ(-1, __cajeta_net_set_ttl(fd, 0, 256));
    __cajeta_net_close(fd);
}

// --- IPV6_V6ONLY on an IPv6 socket sets + reads back ----------------------
// Skipped gracefully if the host cannot create an AF_INET6 socket (rare CI
// without IPv6); otherwise pins the dual-stack toggle round-trip.
TEST(NetOptionsTests, onlyV6SetsAndReadsBack) {
    int32_t fd = makeTcp6();
    if (fd < 0) {
        GTEST_SKIP() << "no IPv6 support on host (socket AF_INET6 failed)";
    }

    ASSERT_EQ(0, __cajeta_net_set_only_v6(fd, 1))
        << "set_only_v6 err=" << __cajeta_net_last_error();
    EXPECT_EQ(1, __cajeta_net_get_only_v6(fd));

    ASSERT_EQ(0, __cajeta_net_set_only_v6(fd, 0));
    EXPECT_EQ(0, __cajeta_net_get_only_v6(fd));

    __cajeta_net_close(fd);
}

// --- every setter/getter rejects a bad fd ---------------------------------
TEST(NetOptionsTests, badFdRejected) {
    EXPECT_EQ(-1, __cajeta_net_set_nodelay(-1, 1));
    EXPECT_EQ(-1, __cajeta_net_get_nodelay(-1));
    EXPECT_EQ(-1, __cajeta_net_set_keepalive(-1, 1));
    EXPECT_EQ(-1, __cajeta_net_get_keepalive(-1));
    EXPECT_EQ(-1, __cajeta_net_set_recvbuf(-1, 4096));
    EXPECT_EQ(-1, __cajeta_net_get_recvbuf(-1));
    EXPECT_EQ(-1, __cajeta_net_set_sendbuf(-1, 4096));
    EXPECT_EQ(-1, __cajeta_net_get_sendbuf(-1));
    EXPECT_EQ(-1, __cajeta_net_set_linger(-1, 1, 5));
    int32_t on = 0, secs = 0;
    EXPECT_EQ(-1, __cajeta_net_get_linger(-1, &on, &secs));
    EXPECT_EQ(-1, __cajeta_net_set_broadcast(-1, 1));
    EXPECT_EQ(-1, __cajeta_net_get_broadcast(-1));
    EXPECT_EQ(-1, __cajeta_net_set_ttl(-1, 0, 64));
    EXPECT_EQ(-1, __cajeta_net_get_ttl(-1, 0));
    EXPECT_EQ(-1, __cajeta_net_set_only_v6(-1, 1));
    EXPECT_EQ(-1, __cajeta_net_get_only_v6(-1));
}
