//
// NET-1.2 — native `sockaddr` marshalling intrinsics.
//
// `__cajeta_net_sockaddr_pack` / `_unpack` bridge the cajeta address types
// (family ordinal + network-order octet buffer + host-order port) and the
// raw platform `sockaddr_in` / `sockaddr_in6` the NET-1.1 socket calls take.
// These golden vectors pin the byte-level marshalling on the native layer;
// the Cajeta-surface IpAddress/SocketAddress parse+format round-trips are
// pinned separately by NetAddressTests (a JIT golden-vector suite).
//
// The test binary links the C runtime natively and cajeta_runtime.c
// #includes cajeta_net_sockaddr.c, so these symbols resolve via extern "C".
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
    // Cajeta AddressFamily ordinals (mirror AddressFamily.cajeta).
    constexpr int32_t CJ_V4 = 0;
    constexpr int32_t CJ_V6 = 1;
}

extern "C" {
    int32_t __cajeta_net_sockaddr_pack(int32_t family, const void* octets,
                                       int32_t port, void* out, int32_t out_cap);
    int32_t __cajeta_net_sockaddr_unpack(const void* addr, int32_t addrlen,
                                         void* octets_out, int32_t* port_out);
    int32_t __cajeta_net_sockaddr_v4_size(void);
    int32_t __cajeta_net_sockaddr_v6_size(void);
    int32_t __cajeta_net_sockaddr_storage_size(void);
}

// --- IPv4 pack produces a correct sockaddr_in -----------------------------
TEST(NetSockaddrTests, packV4ProducesSockaddrIn) {
    // 127.0.0.1 : 8080
    uint8_t octets[4] = {127, 0, 0, 1};
    uint8_t buf[64];
    std::memset(buf, 0, sizeof(buf));

    int32_t n = __cajeta_net_sockaddr_pack(CJ_V4, octets, 8080, buf, (int32_t) sizeof(buf));
    ASSERT_EQ(n, (int32_t) sizeof(sockaddr_in));

    sockaddr_in sa;
    std::memcpy(&sa, buf, sizeof(sa));
    EXPECT_EQ(sa.sin_family, AF_INET);
    EXPECT_EQ(ntohs(sa.sin_port), 8080);
    EXPECT_EQ(ntohl(sa.sin_addr.s_addr), (uint32_t) INADDR_LOOPBACK);
}

// --- IPv4 round-trips pack -> unpack --------------------------------------
TEST(NetSockaddrTests, v4PackUnpackRoundTrips) {
    uint8_t octets[4] = {192, 168, 1, 250};
    uint8_t buf[64];
    int32_t n = __cajeta_net_sockaddr_pack(CJ_V4, octets, 443, buf, (int32_t) sizeof(buf));
    ASSERT_GT(n, 0);

    uint8_t outOct[16];
    std::memset(outOct, 0xAB, sizeof(outOct));
    int32_t port = -1;
    int32_t fam = __cajeta_net_sockaddr_unpack(buf, n, outOct, &port);
    EXPECT_EQ(fam, CJ_V4);
    EXPECT_EQ(port, 443);
    EXPECT_EQ(outOct[0], 192);
    EXPECT_EQ(outOct[1], 168);
    EXPECT_EQ(outOct[2], 1);
    EXPECT_EQ(outOct[3], 250);
}

// --- IPv6 pack produces a correct sockaddr_in6 ----------------------------
TEST(NetSockaddrTests, packV6ProducesSockaddrIn6) {
    // ::1 (loopback): 15 zero bytes then 0x01.
    uint8_t octets[16];
    std::memset(octets, 0, sizeof(octets));
    octets[15] = 1;

    uint8_t buf[64];
    std::memset(buf, 0, sizeof(buf));
    int32_t n = __cajeta_net_sockaddr_pack(CJ_V6, octets, 9443, buf, (int32_t) sizeof(buf));
    ASSERT_EQ(n, (int32_t) sizeof(sockaddr_in6));

    sockaddr_in6 sa;
    std::memcpy(&sa, buf, sizeof(sa));
    EXPECT_EQ(sa.sin6_family, AF_INET6);
    EXPECT_EQ(ntohs(sa.sin6_port), 9443);
    // s6_addr is a 16-byte network-order array; compare byte-for-byte.
    EXPECT_EQ(0, std::memcmp(&sa.sin6_addr, octets, 16));
}

// --- IPv6 round-trips pack -> unpack --------------------------------------
TEST(NetSockaddrTests, v6PackUnpackRoundTrips) {
    // 2001:db8::1  ->  20 01 0d b8 00 00 ... 00 01
    uint8_t octets[16];
    std::memset(octets, 0, sizeof(octets));
    octets[0] = 0x20; octets[1] = 0x01; octets[2] = 0x0d; octets[3] = 0xb8;
    octets[15] = 0x01;

    uint8_t buf[64];
    int32_t n = __cajeta_net_sockaddr_pack(CJ_V6, octets, 8443, buf, (int32_t) sizeof(buf));
    ASSERT_GT(n, 0);

    uint8_t outOct[16];
    int32_t port = -1;
    int32_t fam = __cajeta_net_sockaddr_unpack(buf, n, outOct, &port);
    EXPECT_EQ(fam, CJ_V6);
    EXPECT_EQ(port, 8443);
    EXPECT_EQ(0, std::memcmp(outOct, octets, 16));
}

// --- A sockaddr_in produced by the kernel unpacks correctly ---------------
// Proves unpack reads a real platform-built struct (not just our own pack).
TEST(NetSockaddrTests, unpackReadsKernelBuiltSockaddr) {
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr.s_addr = htonl(0x08080808u);   // 8.8.8.8

    uint8_t outOct[16];
    int32_t port = -1;
    int32_t fam = __cajeta_net_sockaddr_unpack(&sa, (int32_t) sizeof(sa), outOct, &port);
    EXPECT_EQ(fam, CJ_V4);
    EXPECT_EQ(port, 53);
    EXPECT_EQ(outOct[0], 8);
    EXPECT_EQ(outOct[1], 8);
    EXPECT_EQ(outOct[2], 8);
    EXPECT_EQ(outOct[3], 8);
}

// --- Bad arguments are rejected -------------------------------------------
TEST(NetSockaddrTests, packRejectsBadArgs) {
    uint8_t octets[16] = {0};
    uint8_t buf[64];
    // Unknown family.
    EXPECT_EQ(-1, __cajeta_net_sockaddr_pack(7, octets, 80, buf, (int32_t) sizeof(buf)));
    // Out-of-range port.
    EXPECT_EQ(-1, __cajeta_net_sockaddr_pack(CJ_V4, octets, 70000, buf, (int32_t) sizeof(buf)));
    EXPECT_EQ(-1, __cajeta_net_sockaddr_pack(CJ_V4, octets, -1, buf, (int32_t) sizeof(buf)));
    // Undersized output buffer.
    EXPECT_EQ(-1, __cajeta_net_sockaddr_pack(CJ_V4, octets, 80, buf, 1));
    // NULL octets / out.
    EXPECT_EQ(-1, __cajeta_net_sockaddr_pack(CJ_V4, nullptr, 80, buf, (int32_t) sizeof(buf)));
    EXPECT_EQ(-1, __cajeta_net_sockaddr_pack(CJ_V4, octets, 80, nullptr, (int32_t) sizeof(buf)));
}

TEST(NetSockaddrTests, unpackRejectsUnknownFamily) {
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = 0xAB;   // not AF_INET/AF_INET6
    uint8_t outOct[16];
    int32_t port = -1;
    EXPECT_EQ(-1, __cajeta_net_sockaddr_unpack(&sa, (int32_t) sizeof(sa), outOct, &port));
}

// --- Size helpers report the platform struct sizes ------------------------
TEST(NetSockaddrTests, sizeHelpersMatchPlatform) {
    EXPECT_EQ(__cajeta_net_sockaddr_v4_size(), (int32_t) sizeof(sockaddr_in));
    EXPECT_EQ(__cajeta_net_sockaddr_v6_size(), (int32_t) sizeof(sockaddr_in6));
    EXPECT_EQ(__cajeta_net_sockaddr_storage_size(), (int32_t) sizeof(sockaddr_storage));
    // Storage must be large enough to hold either concrete struct.
    EXPECT_GE(__cajeta_net_sockaddr_storage_size(), __cajeta_net_sockaddr_v4_size());
    EXPECT_GE(__cajeta_net_sockaddr_storage_size(), __cajeta_net_sockaddr_v6_size());
}
