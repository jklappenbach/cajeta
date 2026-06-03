//
// NET-1.5 — UdpSocket: the connected-UDP datagram path.
//
// NET-1.1's NetSocketTests already pins the *unconnected* UDP path
// (sendto / recvfrom + sender-address reporting:
// udpLoopbackRecvFromReportsPeer). NET-1.5 adds the *connected* UDP form
// that `UdpSocket.connect` / `send` / `recv` lower to — UDP `connect`
// performs no handshake; it records a default peer so the kernel (a)
// filters inbound datagrams to that peer and (b) accepts the address-less
// send()/recv() forms. These golden vectors pin that behaviour at the
// native intrinsic layer, exactly the way NetSocketTests pins the blocking
// TCP/UDP primitives before the `cajeta.net` surface (UdpSocket) is wired
// by the compiler's net-receiver lowering (shared with NET-1.3 / NET-1.4).
//
// The test binary links the C runtime natively and cajeta_runtime.c
// #includes cajeta_net_socket.c, so these `__cajeta_net_*` symbols resolve
// directly via extern "C" (same setup NetSocketTests documents).
//
// Pinned:
//   udpConnectedSendRecvRoundTrips     → connect → send → recv over loopback
//   udpConnectedRecvFiltersForeignPeer → a datagram from a non-default peer
//                                         is not delivered to a connected recv
//   nonblockingRecvOnConnectedUdpWouldBlock → connected, empty → WOULDBLOCK
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

// NET-1.1 cross-platform error ordinals (mirror cajeta_net_socket.c).
namespace {
    constexpr int32_t NET_WOULDBLOCK = 1;
}

extern "C" {
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len, int32_t flags);
    int64_t __cajeta_net_recv(int32_t fd, void* buf, int64_t len, int32_t flags);
    int64_t __cajeta_net_sendto(int32_t fd, const void* buf, int64_t len,
                                int32_t flags, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_set_nonblocking(int32_t fd, int32_t nonblocking);
    int32_t __cajeta_net_last_error(void);
}

namespace {

// A loopback (127.0.0.1) sockaddr_in for `port` (host byte order).
// port == 0 → kernel picks an ephemeral port at bind time.
sockaddr_in loopbackV4(uint16_t port) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

// Read back the kernel-assigned port after a bind(..,:0). getsockname is
// NET-1.3's localAddress() intrinsic; called directly here as the surface
// is not yet wired.
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

// A connected-UDP recv on loopback can momentarily report WOULDBLOCK
// before the kernel delivers a just-sent datagram; spin briefly so the
// test is deterministic without blocking forever on a genuine failure.
int64_t recvSpin(int32_t fd, void* buf, int64_t cap) {
    for (int i = 0; i < 1000; ++i) {
        int64_t n = __cajeta_net_recv(fd, buf, cap, 0);
        if (n >= 0) return n;
        if (__cajeta_net_last_error() != NET_WOULDBLOCK) return n;
    }
    return -1;
}

}  // namespace

// --- connected UDP: connect → send → recv round-trips ----------------------
//
// Two UDP sockets on loopback. Each connect()s to the other's bound port so
// each has a default peer, then the address-less send()/recv() forms move a
// datagram both ways. Pins UdpSocket.connect/send/recv at the native layer.
TEST(NetUdpSocketTests, udpConnectedSendRecvRoundTrips) {
    int32_t a = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    int32_t b = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);

    sockaddr_in aBind = loopbackV4(0);
    sockaddr_in bBind = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(a, &aBind, (int32_t) sizeof(aBind)));
    ASSERT_EQ(0, __cajeta_net_bind(b, &bBind, (int32_t) sizeof(bBind)));
    uint16_t aPort = boundPort(a);
    uint16_t bPort = boundPort(b);
    ASSERT_NE(aPort, 0);
    ASSERT_NE(bPort, 0);

    // Connect each socket to the other (set default peer; no handshake).
    sockaddr_in aPeer = loopbackV4(bPort);   // a's peer is b
    sockaddr_in bPeer = loopbackV4(aPort);   // b's peer is a
    ASSERT_EQ(0, __cajeta_net_connect(a, &aPeer, (int32_t) sizeof(aPeer)))
        << "connect err=" << __cajeta_net_last_error();
    ASSERT_EQ(0, __cajeta_net_connect(b, &bPeer, (int32_t) sizeof(bPeer)))
        << "connect err=" << __cajeta_net_last_error();

    // a --send--> b, using the connected (address-less) form.
    const char msg[] = "udp-connected";
    int64_t sent = __cajeta_net_send(a, msg, (int64_t) sizeof(msg), 0);
    ASSERT_EQ(sent, (int64_t) sizeof(msg));

    char buf[64];
    int64_t got = recvSpin(b, buf, (int64_t) sizeof(buf));
    ASSERT_EQ(got, (int64_t) sizeof(msg));
    EXPECT_EQ(0, std::memcmp(msg, buf, sizeof(msg)));

    // And the reverse direction: b --send--> a.
    const char reply[] = "udp-reply";
    ASSERT_EQ((int64_t) sizeof(reply),
              __cajeta_net_send(b, reply, (int64_t) sizeof(reply), 0));
    char back[64];
    int64_t backN = recvSpin(a, back, (int64_t) sizeof(back));
    ASSERT_EQ(backN, (int64_t) sizeof(reply));
    EXPECT_EQ(0, std::memcmp(reply, back, sizeof(reply)));

    __cajeta_net_close(a);
    __cajeta_net_close(b);
}

// --- connected UDP filters out datagrams from a non-default peer -----------
//
// A connected UDP socket only delivers datagrams from its default peer. A
// datagram from a *different* sender must NOT be delivered to a connected
// recv — proving connect() actually set a peer filter, the defining
// difference from the unconnected recvfrom path.
TEST(NetUdpSocketTests, udpConnectedRecvFiltersForeignPeer) {
    int32_t rcv = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    int32_t peer = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    int32_t stranger = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(rcv, 0);
    ASSERT_GE(peer, 0);
    ASSERT_GE(stranger, 0);

    sockaddr_in rBind = loopbackV4(0);
    sockaddr_in pBind = loopbackV4(0);
    sockaddr_in sBind = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(rcv, &rBind, (int32_t) sizeof(rBind)));
    ASSERT_EQ(0, __cajeta_net_bind(peer, &pBind, (int32_t) sizeof(pBind)));
    ASSERT_EQ(0, __cajeta_net_bind(stranger, &sBind, (int32_t) sizeof(sBind)));
    uint16_t rPort = boundPort(rcv);
    uint16_t pPort = boundPort(peer);
    ASSERT_NE(rPort, 0);
    ASSERT_NE(pPort, 0);

    // rcv connects to `peer` only.
    sockaddr_in rPeer = loopbackV4(pPort);
    ASSERT_EQ(0, __cajeta_net_connect(rcv, &rPeer, (int32_t) sizeof(rPeer)));

    sockaddr_in rDst = loopbackV4(rPort);

    // The stranger sends first; on a connected socket this datagram must be
    // dropped (ICMP-refused / filtered), never delivered to recv().
    const char bad[] = "from-stranger";
    ASSERT_EQ((int64_t) sizeof(bad),
              __cajeta_net_sendto(stranger, bad, (int64_t) sizeof(bad), 0,
                                  &rDst, (int32_t) sizeof(rDst)));

    // The real peer then sends the legitimate datagram.
    const char good[] = "from-peer";
    ASSERT_EQ((int64_t) sizeof(good),
              __cajeta_net_sendto(peer, good, (int64_t) sizeof(good), 0,
                                  &rDst, (int32_t) sizeof(rDst)));

    // recv() on the connected socket must return the peer's datagram. The
    // stranger's must have been filtered, so the FIRST delivered datagram is
    // "from-peer", not "from-stranger".
    char buf[64];
    int64_t got = recvSpin(rcv, buf, (int64_t) sizeof(buf));
    ASSERT_EQ(got, (int64_t) sizeof(good));
    EXPECT_EQ(0, std::memcmp(good, buf, sizeof(good)))
        << "connected UDP recv delivered a datagram from a non-default peer";

    __cajeta_net_close(rcv);
    __cajeta_net_close(peer);
    __cajeta_net_close(stranger);
}

// --- non-blocking recv on a connected, empty UDP socket → WouldBlock -------
//
// The reactor (Phase 3) drives the connected-UDP recv via the same
// WouldBlock-as-a-value contract the TCP path uses (NET-1.7). Pin it: a
// non-blocking recv on a connected socket with nothing queued returns -1
// with last-error == WOULDBLOCK, not a hard error and not data.
TEST(NetUdpSocketTests, nonblockingRecvOnConnectedUdpWouldBlock) {
    int32_t a = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    int32_t b = __cajeta_net_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);

    sockaddr_in aBind = loopbackV4(0);
    sockaddr_in bBind = loopbackV4(0);
    ASSERT_EQ(0, __cajeta_net_bind(a, &aBind, (int32_t) sizeof(aBind)));
    ASSERT_EQ(0, __cajeta_net_bind(b, &bBind, (int32_t) sizeof(bBind)));
    uint16_t bPort = boundPort(b);
    ASSERT_NE(bPort, 0);

    sockaddr_in aPeer = loopbackV4(bPort);
    ASSERT_EQ(0, __cajeta_net_connect(a, &aPeer, (int32_t) sizeof(aPeer)));
    ASSERT_EQ(0, __cajeta_net_set_nonblocking(a, 1));

    char buf[16];
    int64_t got = __cajeta_net_recv(a, buf, (int64_t) sizeof(buf), 0);
    ASSERT_EQ(got, -1) << "empty non-blocking connected UDP recv returned data";
    EXPECT_EQ(NET_WOULDBLOCK, __cajeta_net_last_error());

    __cajeta_net_close(a);
    __cajeta_net_close(b);
}
