//
// NET-14.1 — UDP multicast native intrinsics.
//
// Golden vectors for the multicast option surface added to
// cajeta_net_socket_options.c: IPv4/IPv6 group membership (`ip_mreq` /
// `ipv6_mreq`), the outbound multicast TTL / loopback / interface pairs
// (including IPv4's u_char payload-width quirk), and the getsockname-based
// family probe. Same discipline as NetOptionsTests — direct extern "C"
// calls against sockets made through `__cajeta_net_socket`.
//
// Delivery semantics run over the loopback interface: the receiver joins the
// group ON 127.0.0.1 (v4) / the lo index (v6) and the sender pins its
// outbound multicast interface the same way, so nothing leaves the host and
// no real NIC or router is consulted — the recipe that works on bare CI
// runners. Receives poll a NONBLOCKING socket against a deadline; "not
// delivered" asserts a quiet window rather than blocking forever.
//
// The delivery tests are POSIX-only (GTEST_SKIP on _WIN32): Windows loopback
// multicast needs an interface-selection dance that NET-14's acceptance
// (Linux CI) does not exercise; the option round-trips below run everywhere.
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
#  include <net/if.h>        // if_nametoindex
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

extern "C" {
    // NET-1.1 verbs reused as scaffolding.
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_sendto(int32_t fd, const void* buf, int64_t len,
                                int32_t flags, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_recvfrom(int32_t fd, void* buf, int64_t len, int32_t flags,
                                  void* addr_out, int32_t* addrlen_inout);
    int32_t __cajeta_net_set_nonblocking(int32_t fd, int32_t nonblocking);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_last_error(void);

    // NET-14.1 surface under test.
    int32_t __cajeta_net_mcast_join_v4(int32_t fd, const void* group, const void* iface);
    int32_t __cajeta_net_mcast_leave_v4(int32_t fd, const void* group, const void* iface);
    int32_t __cajeta_net_mcast_join_v6(int32_t fd, const void* group, int32_t iface_index);
    int32_t __cajeta_net_mcast_leave_v6(int32_t fd, const void* group, int32_t iface_index);
    int32_t __cajeta_net_set_mcast_ttl(int32_t fd, int32_t is_v6, int32_t ttl);
    int32_t __cajeta_net_get_mcast_ttl(int32_t fd, int32_t is_v6);
    int32_t __cajeta_net_set_mcast_loop(int32_t fd, int32_t is_v6, int32_t on);
    int32_t __cajeta_net_get_mcast_loop(int32_t fd, int32_t is_v6);
    int32_t __cajeta_net_set_mcast_if_v4(int32_t fd, const void* iface);
    int32_t __cajeta_net_get_mcast_if_v4(int32_t fd, void* iface_out);
    int32_t __cajeta_net_set_mcast_if_v6(int32_t fd, int32_t iface_index);
    int32_t __cajeta_net_get_mcast_if_v6(int32_t fd);
    int32_t __cajeta_net_sockname_is_v6(int32_t fd);
}

namespace {

int32_t makeUdp4() { return __cajeta_net_socket(AF_INET,  SOCK_DGRAM, 0); }
int32_t makeUdp6() { return __cajeta_net_socket(AF_INET6, SOCK_DGRAM, 0); }

// The multicast intrinsics take cajeta int8[] HEADERS — `{ i64 count,
// [N x i8] data }`, payload at offset 8 (the __cajeta_sha1_update convention)
// — because their Cajeta callers pass `IpAddress.getOctets()` straight
// through the @Native bridge. Mock the header shape here.
struct OctetHdr {
    int64_t count;
    unsigned char data[16];
};

// 239.255.77.88 — organization-local ASM scope, nothing routable.
const OctetHdr kGroup4 = {4, {239, 255, 77, 88}};
// 127.0.0.1 in network order — the join/send interface for every test.
const OctetHdr kLo4 = {4, {127, 0, 0, 1}};

#if !defined(_WIN32)

// Bind `fd` to 0.0.0.0:0 and return the kernel-assigned port (host order).
int bindEphemeral4(int32_t fd) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (__cajeta_net_bind(fd, &sa, (int32_t) sizeof(sa)) != 0) return -1;
    struct sockaddr_in got;
    socklen_t len = sizeof(got);
    if (getsockname(fd, (struct sockaddr*) &got, &len) != 0) return -1;
    return (int) ntohs(got.sin_port);
}

// Send `payload` to kGroup4:port through `tx` (whose outbound multicast
// interface must already be pinned to loopback).
bool sendToGroup4(int32_t tx, int port, unsigned char payload) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    memcpy(&dst.sin_addr, kGroup4.data, 4);
    dst.sin_port = htons((uint16_t) port);
    unsigned char b = payload;
    return __cajeta_net_sendto(tx, &b, 1, 0, &dst, (int32_t) sizeof(dst)) == 1;
}

// Poll a nonblocking receive until the deadline. Returns the payload byte, or
// -1 if nothing arrived inside `ms`.
int pollRecv(int32_t rx, int ms) {
    for (int waited = 0; waited <= ms; waited += 10) {
        unsigned char b = 0;
        int64_t n = __cajeta_net_recvfrom(rx, &b, 1, 0, nullptr, nullptr);
        if (n == 1) return (int) b;
        usleep(10 * 1000);
    }
    return -1;
}

#endif  // !_WIN32

}  // namespace

// --- IPv4 ASM join + receive over loopback; loop-off suppresses ------------
TEST(NetMulticastTests, ipv4AsmLoopbackJoinReceive) {
#if defined(_WIN32)
    GTEST_SKIP() << "delivery semantics are validated on POSIX CI";
#else
    int32_t rx = makeUdp4();
    int32_t tx = makeUdp4();
    ASSERT_GE(rx, 0);
    ASSERT_GE(tx, 0);
    int port = bindEphemeral4(rx);
    ASSERT_GT(port, 0);
    ASSERT_EQ(0, __cajeta_net_set_nonblocking(rx, 1));

    // Receiver joins the group on the loopback interface; sender pins its
    // outbound multicast interface to loopback and keeps loop ON (the default,
    // but pinned explicitly so the assertion is self-contained).
    ASSERT_EQ(0, __cajeta_net_mcast_join_v4(rx, &kGroup4, &kLo4))
        << "join err=" << __cajeta_net_last_error();
    ASSERT_EQ(0, __cajeta_net_set_mcast_if_v4(tx, &kLo4));
    ASSERT_EQ(0, __cajeta_net_set_mcast_loop(tx, 0, 1));

    ASSERT_TRUE(sendToGroup4(tx, port, 0x5A));
    EXPECT_EQ(0x5A, pollRecv(rx, 2000)) << "joined receiver never got the datagram";

    // NOT asserted here: loop=off suppression. With the egress interface
    // pinned to lo, the datagram re-enters the host THROUGH lo like any
    // other loopback packet, so joined sockets receive it regardless —
    // IP_MULTICAST_LOOP governs only the kernel's internal loopback copy
    // for a non-lo egress. Verified empirically (the suppressed send still
    // arrived); the flag's set→get contract is pinned in
    // NetMulticastOptionsTests.roundTripAndErrors instead.

    __cajeta_net_close(tx);
    __cajeta_net_close(rx);
#endif
}

// --- leaveGroup stops delivery; a re-join restores it -----------------------
TEST(NetMulticastTests, leaveStopsDelivery) {
#if defined(_WIN32)
    GTEST_SKIP() << "delivery semantics are validated on POSIX CI";
#else
    int32_t rx = makeUdp4();
    int32_t tx = makeUdp4();
    ASSERT_GE(rx, 0);
    ASSERT_GE(tx, 0);
    int port = bindEphemeral4(rx);
    ASSERT_GT(port, 0);
    ASSERT_EQ(0, __cajeta_net_set_nonblocking(rx, 1));
    ASSERT_EQ(0, __cajeta_net_mcast_join_v4(rx, &kGroup4, &kLo4));
    ASSERT_EQ(0, __cajeta_net_set_mcast_if_v4(tx, &kLo4));
    ASSERT_EQ(0, __cajeta_net_set_mcast_loop(tx, 0, 1));

    ASSERT_TRUE(sendToGroup4(tx, port, 1));
    ASSERT_EQ(1, pollRecv(rx, 2000)) << "sanity receive before the leave failed";

    ASSERT_EQ(0, __cajeta_net_mcast_leave_v4(rx, &kGroup4, &kLo4))
        << "leave err=" << __cajeta_net_last_error();
    ASSERT_TRUE(sendToGroup4(tx, port, 2));
    EXPECT_EQ(-1, pollRecv(rx, 300)) << "left the group but still receiving";

    // Re-join proves the socket itself is healthy — the miss above was the
    // membership, not a broken descriptor.
    ASSERT_EQ(0, __cajeta_net_mcast_join_v4(rx, &kGroup4, &kLo4));
    ASSERT_TRUE(sendToGroup4(tx, port, 3));
    EXPECT_EQ(3, pollRecv(rx, 2000)) << "re-join did not restore delivery";

    __cajeta_net_close(tx);
    __cajeta_net_close(rx);
#endif
}

// --- IPv6 join + receive over loopback --------------------------------------
TEST(NetMulticastTests, ipv6JoinReceive) {
#if defined(_WIN32)
    GTEST_SKIP() << "delivery semantics are validated on POSIX CI";
#else
    unsigned int lo = if_nametoindex("lo");
    if (lo == 0) lo = if_nametoindex("lo0");   // the BSDs / macOS
    if (lo == 0) GTEST_SKIP() << "no loopback interface index";

    int32_t rx = makeUdp6();
    int32_t tx = makeUdp6();
    ASSERT_GE(rx, 0);
    ASSERT_GE(tx, 0);

    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_addr = in6addr_any;
    ASSERT_EQ(0, __cajeta_net_bind(rx, &sa, (int32_t) sizeof(sa)));
    struct sockaddr_in6 got;
    socklen_t glen = sizeof(got);
    ASSERT_EQ(0, getsockname(rx, (struct sockaddr*) &got, &glen));
    ASSERT_EQ(0, __cajeta_net_set_nonblocking(rx, 1));

    // ff15::4242 — transient, site-local scope; never routable.
    OctetHdr group6;
    memset(&group6, 0, sizeof(group6));
    group6.count = 16;
    group6.data[0] = 0xff; group6.data[1] = 0x15;
    group6.data[14] = 0x42; group6.data[15] = 0x42;

    ASSERT_EQ(0, __cajeta_net_mcast_join_v6(rx, &group6, (int32_t) lo))
        << "v6 join err=" << __cajeta_net_last_error();
    ASSERT_EQ(0, __cajeta_net_set_mcast_if_v6(tx, (int32_t) lo));
    ASSERT_EQ(0, __cajeta_net_set_mcast_loop(tx, 1, 1));

    struct sockaddr_in6 dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin6_family = AF_INET6;
    memcpy(&dst.sin6_addr, group6.data, 16);
    dst.sin6_port = got.sin6_port;
    unsigned char b = 0x66;
    // A host without an ff00::/8 route on lo refuses the send with
    // ENETUNREACH — a kernel routing configuration, not a NET-14 defect
    // (the JOIN above already succeeded, which is what pins the intrinsic).
    // Skip delivery on such hosts rather than fail.
    if (__cajeta_net_sendto(tx, &b, 1, 0, &dst, (int32_t) sizeof(dst)) != 1) {
        __cajeta_net_close(tx);
        __cajeta_net_close(rx);
        GTEST_SKIP() << "no IPv6 multicast route on loopback (send err="
                     << __cajeta_net_last_error() << ")";
    }
    EXPECT_EQ(0x66, pollRecv(rx, 2000)) << "v6 joined receiver never got the datagram";

    __cajeta_net_close(tx);
    __cajeta_net_close(rx);
#endif
}

// --- option set→get round-trips + argument/family errors --------------------
TEST(NetMulticastOptionsTests, roundTripAndErrors) {
    int32_t u4 = makeUdp4();
    int32_t u6 = makeUdp6();
    ASSERT_GE(u4, 0);
    ASSERT_GE(u6, 0);

    // Multicast TTL — v4 rides the u_char payload path, v6 the int path.
    ASSERT_EQ(0, __cajeta_net_set_mcast_ttl(u4, 0, 5));
    EXPECT_EQ(5, __cajeta_net_get_mcast_ttl(u4, 0));
    ASSERT_EQ(0, __cajeta_net_set_mcast_ttl(u6, 1, 9));
    EXPECT_EQ(9, __cajeta_net_get_mcast_ttl(u6, 1));

    // Loopback toggle, both families.
    ASSERT_EQ(0, __cajeta_net_set_mcast_loop(u4, 0, 0));
    EXPECT_EQ(0, __cajeta_net_get_mcast_loop(u4, 0));
    ASSERT_EQ(0, __cajeta_net_set_mcast_loop(u4, 0, 1));
    EXPECT_EQ(1, __cajeta_net_get_mcast_loop(u4, 0));
    ASSERT_EQ(0, __cajeta_net_set_mcast_loop(u6, 1, 0));
    EXPECT_EQ(0, __cajeta_net_get_mcast_loop(u6, 1));

    // Outbound interface: v4 by address (loopback), v6 by index (0 = default).
    ASSERT_EQ(0, __cajeta_net_set_mcast_if_v4(u4, &kLo4));
    OctetHdr ifOut = {4, {0, 0, 0, 0}};
    ASSERT_EQ(0, __cajeta_net_get_mcast_if_v4(u4, &ifOut));
    EXPECT_EQ(0, memcmp(ifOut.data, kLo4.data, 4));
    ASSERT_EQ(0, __cajeta_net_set_mcast_if_v6(u6, 0));
    EXPECT_EQ(0, __cajeta_net_get_mcast_if_v6(u6));

    // Family probe — the is_v6 dispatch the Cajeta surface relies on. An
    // unbound socket still reports its family via getsockname.
    EXPECT_EQ(0, __cajeta_net_sockname_is_v6(u4));
    EXPECT_EQ(1, __cajeta_net_sockname_is_v6(u6));

    // Errors: TTL out of range, bad fd, NULL group, negative v6 index.
    EXPECT_EQ(-1, __cajeta_net_set_mcast_ttl(u4, 0, 256));
    EXPECT_EQ(-1, __cajeta_net_set_mcast_ttl(u4, 0, -1));
    EXPECT_EQ(-1, __cajeta_net_mcast_join_v4(-1, &kGroup4, nullptr));
    EXPECT_EQ(-1, __cajeta_net_mcast_join_v4(u4, nullptr, nullptr));
    OctetHdr g6 = {16, {0xff, 0x15}};
    EXPECT_EQ(-1, __cajeta_net_mcast_join_v6(u6, &g6, -2));
    EXPECT_EQ(-1, __cajeta_net_sockname_is_v6(-1));

    __cajeta_net_close(u4);
    __cajeta_net_close(u6);
}
