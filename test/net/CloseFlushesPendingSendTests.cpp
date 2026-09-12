//
// CloseFlushesPendingSendTests.cpp — `__cajeta_net_close` must deliver what is
// already queued to send, on every host.
//
// The http server answers a stalled peer (slowloris) with 408 and then closes.
// On Windows the peer read NOTHING: Winsock answers a close() that still has
// unread input with an RST, and an RST discards the send buffer. Linux delivers
// the bytes. Same code, same call, opposite outcome — so `TcpStream.close()`
// was not a portable operation, and every error response http writes on a
// stalled connection (408, 413, 431) was lost there.
//
// The fix is a half-close of the write side before the close, which flushes and
// sends FIN instead of RST. Measured over a raw socket pair, server writes 51
// bytes and closes while 200 bytes from the peer sit unread:
//
//                              linux   windows
//     close()                   51 B      0 B     <- the bug
//     shutdown(WR), close()     51 B     51 B     <- the fix
//
// This test is the in-binary form of that probe. It is a native-ABI test rather
// than a JIT one on purpose: the behaviour lives entirely in
// `__cajeta_net_close`, and driving it through the cajeta surface would add a
// scheduler between the assertion and the thing asserted.
//
// It passes on Linux either way — Linux never had the bug. Its value is the
// Windows leg, and it is written to be meaningful there rather than skipped.
//

#include "gtest/gtest.h"

#include "net/LoopbackFixtures.h"   // cross-platform socket shims + WSAStartup

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

extern "C" {
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_close(int32_t fd);
    int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len, int32_t flags);
}

namespace {

constexpr const char* RESPONSE = "HTTP/1.1 408 Request Timeout\r\ncontent-length: 0\r\n\r\n";

// A connected pair: `*rt` is the RUNTIME-side fd (the one whose close is under
// test — it plays the server), `*peer` is a raw socket the test drives directly
// (it plays the stalled client). The two handle spaces are never mixed, the
// same split ReactorHarness::makePair uses.
bool makePair(int32_t* rt, cajeta_net_test_socket_t* peer) {
    using namespace cajeta::net::testing;  // ensures Winsock is up on Windows
    cajeta_net_test_socket_t lst = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lst == CAJETA_NET_TEST_BAD_SOCKET) return false;
    int one = 1;
    ::setsockopt(lst, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(lst, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        CAJETA_NET_TEST_CLOSESOCK(lst); return false;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(lst, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        CAJETA_NET_TEST_CLOSESOCK(lst); return false;
    }
    if (::listen(lst, 4) < 0) { CAJETA_NET_TEST_CLOSESOCK(lst); return false; }

    // The runtime fd dials; the raw socket accepts. Which end accepted does not
    // matter to the kernel, only which end has unread data when it closes.
    int32_t c = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0) { CAJETA_NET_TEST_CLOSESOCK(lst); return false; }
    if (__cajeta_net_connect(c, &addr, (int32_t) sizeof(addr)) != 0) {
        __cajeta_net_close(c); CAJETA_NET_TEST_CLOSESOCK(lst); return false;
    }
    cajeta_net_test_socket_t p = ::accept(lst, nullptr, nullptr);
    CAJETA_NET_TEST_CLOSESOCK(lst);
    if (p == CAJETA_NET_TEST_BAD_SOCKET) { __cajeta_net_close(c); return false; }

    *rt = c;
    *peer = p;
    return true;
}

} // namespace

// The bug, reproduced at the exact shape http hits: the peer is mid-request and
// still sending, the server answers and closes without draining, and the
// response must still arrive.
TEST(CloseFlushesPendingSendTests, closeDeliversQueuedBytesDespiteUnreadInput) {
    int32_t rt = -1;
    cajeta_net_test_socket_t peer = CAJETA_NET_TEST_BAD_SOCKET;
    ASSERT_TRUE(makePair(&rt, &peer)) << "could not stand up a loopback pair";

    // The stalled client drips a partial request and never finishes it. These
    // bytes are what the server will NOT have read when it closes — the
    // precondition for the RST, and the whole point of the test.
    for (int i = 0; i < 200; i++) {
        ASSERT_EQ(::send(peer, "y", 1, 0), 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // The server gives up, answers 408, and closes. It deliberately never
    // recv()s, so the 200 bytes above are still queued on its side.
    const int64_t n = (int64_t) std::strlen(RESPONSE);
    ASSERT_EQ(__cajeta_net_send(rt, RESPONSE, n, 0), n);
    ASSERT_EQ(__cajeta_net_close(rt), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    char buf[512];
    int total = 0;
    for (;;) {
        int r = ::recv(peer, buf + total, (int) (sizeof(buf) - 1 - (size_t) total), 0);
        if (r <= 0) break;
        total += r;
        if (total >= (int) sizeof(buf) - 1) break;
    }
    buf[total > 0 ? total : 0] = '\0';
    CAJETA_NET_TEST_CLOSESOCK(peer);

    // Without the half-close this reads 0 on Windows: the RST discarded a
    // response the server had already handed to the kernel.
    EXPECT_EQ(total, (int) n)
        << "peer read " << total << " of " << n << " bytes — close() discarded "
           "a queued response (RST instead of FIN?)";
    EXPECT_STREQ(buf, RESPONSE);
}

// The ordinary close still works: nothing unread, response arrives, and the
// peer sees a clean EOF rather than a reset. Guards against "fixing" the case
// above by suppressing the close.
TEST(CloseFlushesPendingSendTests, closeStillSignalsCleanEofWhenNothingUnread) {
    int32_t rt = -1;
    cajeta_net_test_socket_t peer = CAJETA_NET_TEST_BAD_SOCKET;
    ASSERT_TRUE(makePair(&rt, &peer)) << "could not stand up a loopback pair";

    const int64_t n = (int64_t) std::strlen(RESPONSE);
    ASSERT_EQ(__cajeta_net_send(rt, RESPONSE, n, 0), n);
    ASSERT_EQ(__cajeta_net_close(rt), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    char buf[512];
    int total = 0;
    for (;;) {
        int r = ::recv(peer, buf + total, (int) (sizeof(buf) - 1 - (size_t) total), 0);
        if (r <= 0) break;                    // 0 == the FIN we want to see
        total += r;
        if (total >= (int) sizeof(buf) - 1) break;
    }
    buf[total > 0 ? total : 0] = '\0';
    CAJETA_NET_TEST_CLOSESOCK(peer);

    EXPECT_EQ(total, (int) n);
    EXPECT_STREQ(buf, RESPONSE);
}
