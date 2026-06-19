//
// LoopbackFixturesTests.cpp — self-validation of the NET-13.1 loopback
// fixtures (test/net/LoopbackFixtures.h).
//
// NET-13.1 ships *test infrastructure* (a reusable TCP echo + HTTP
// loopback server) that later phases (NET-3 reactor harness, NET-8 HTTP
// client, NET-9 server) connect to as a real peer. Those phases don't
// exist yet, so — exactly like the NET-13.2 golden-corpus meta-test —
// this suite validates the **fixture itself**: that a client can bind,
// connect, send, and receive against it over loopback and get the
// contract the fixture promises (byte-exact echo; routed HTTP responses;
// per-path hit counts; captured POST bodies; a 404 for an unknown path).
//
// Crucially, the *client* half of every round-trip here is driven through
// the product's own NET-1.1 socket intrinsics (`__cajeta_net_socket` /
// `_connect` / `_send` / `_recv` / `_close`) — the same symbols
// `TcpStream` (NET-1.3) lowers to. So this both keeps the fixture honest
// and demonstrates the fixture is a valid peer for a cajeta.io.net client,
// which is its reason to exist. (NetSocketTests/NetListenerTests already
// pin those intrinsics directly; here they are the test *driver*, not the
// subject.)
//
#include "gtest/gtest.h"

#include "net/LoopbackFixtures.h"

#include <cstdint>
#include <cstring>
#include <string>

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

using namespace cajeta::net::testing;

// The product's NET-1.1 socket intrinsics — used here as the *client*
// driver, resolved via extern "C" against the natively-linked runtime
// (cajeta_runtime.c #includes cajeta_net_socket.c), the same way
// NetSocketTests reaches them.
extern "C" {
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len, int32_t flags);
    int64_t __cajeta_net_recv(int32_t fd, void* buf, int64_t len, int32_t flags);
    int32_t __cajeta_net_shutdown(int32_t fd, int32_t how);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_last_error(void);
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

// Open a cajeta-intrinsic client socket connected to the fixture's port.
int32_t connectClient(uint16_t port) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0) << "socket err=" << __cajeta_net_last_error();
    sockaddr_in addr = loopbackV4(port);
    EXPECT_EQ(0, __cajeta_net_connect(fd, &addr, (int32_t) sizeof(addr)))
        << "connect err=" << __cajeta_net_last_error();
    return fd;
}

// Read exactly `n` bytes from `fd` via the intrinsics (looping over short
// reads), or fewer on early EOF; returns what was read.
std::string recvN(int32_t fd, size_t n) {
    std::string out;
    char buf[4096];
    while (out.size() < n) {
        size_t want = n - out.size();
        int64_t cap = want > sizeof(buf) ? (int64_t) sizeof(buf) : (int64_t) want;
        int64_t r = __cajeta_net_recv(fd, buf, cap, 0);
        if (r <= 0) break;   // 0 = EOF, <0 = error
        out.append(buf, static_cast<size_t>(r));
    }
    return out;
}

// Read until the peer closes (orderly EOF) — the response delimiter for the
// fixture's `Connection: close` HTTP responses.
std::string recvToEof(int32_t fd) {
    std::string out;
    char buf[4096];
    for (;;) {
        int64_t r = __cajeta_net_recv(fd, buf, (int64_t) sizeof(buf), 0);
        if (r <= 0) break;
        out.append(buf, static_cast<size_t>(r));
    }
    return out;
}

}  // namespace

// --- TcpEchoServer: a byte payload round-trips byte-identically ------------
// The core fixture contract + the peer the NET-1.3 TcpStream round-trip and
// the NET-3 reactor harness rely on: send N bytes, read the same N back.
TEST(LoopbackFixtures, tcpEchoRoundTripsPayload) {
    TcpEchoServer server;
    ASSERT_NE(server.port(), 0);

    int32_t fd = connectClient(server.port());
    ASSERT_GE(fd, 0);

    const std::string payload = "the quick brown fox \x01\x02\x03 jumps";
    int64_t sent = __cajeta_net_send(fd, payload.data(),
                                     (int64_t) payload.size(), 0);
    ASSERT_EQ(sent, (int64_t) payload.size())
        << "send err=" << __cajeta_net_last_error();

    std::string echoed = recvN(fd, payload.size());
    EXPECT_EQ(echoed, payload) << "echo payload mismatch";

    __cajeta_net_close(fd);
}

// --- TcpEchoServer: multiple sequential connections each echo --------------
// The fixture serves connection after connection (the accept loop survives a
// peer close), and counts each — what a multi-request / keep-alive-vs-new
// test reads back.
TEST(LoopbackFixtures, tcpEchoServesSequentialConnections) {
    TcpEchoServer server;

    for (int i = 0; i < 3; ++i) {
        int32_t fd = connectClient(server.port());
        ASSERT_GE(fd, 0);
        char c = (char) ('A' + i);
        ASSERT_EQ(1, __cajeta_net_send(fd, &c, 1, 0));
        std::string back = recvN(fd, 1);
        ASSERT_EQ(back.size(), 1u);
        EXPECT_EQ(back[0], c);
        __cajeta_net_close(fd);
    }

    // The echo server accepted exactly the three connections we opened.
    EXPECT_EQ(server.acceptedConnections(), 3);
}

// --- TcpEchoServer: half-close (shutdown write) yields EOF echo end --------
// Half-closing the client write side sends the server EOF; the server's recv
// returns 0 and it stops — the client's read of the echo then sees orderly
// EOF after the echoed bytes. Pins the fixture's EOF handling, which the
// reactor read-loop tests exercise.
TEST(LoopbackFixtures, tcpEchoHandlesHalfClose) {
    TcpEchoServer server;
    int32_t fd = connectClient(server.port());
    ASSERT_GE(fd, 0);

    const std::string payload = "half-close";
    ASSERT_EQ((int64_t) payload.size(),
              __cajeta_net_send(fd, payload.data(), (int64_t) payload.size(), 0));
    // SHUT_WR — tell the server we're done writing.
    ASSERT_EQ(0, __cajeta_net_shutdown(fd, 1));

    std::string all = recvToEof(fd);
    EXPECT_EQ(all, payload) << "echoed bytes before EOF mismatch";

    __cajeta_net_close(fd);
}

// --- LoopbackHttpServer: a routed GET returns the canned response ----------
// The HTTP fixture's reason to exist: register a route, GET it through a
// cajeta-intrinsic client socket, and read back the status + body — the peer
// NET-8's httpsGetReturnsBody-style tests connect to.
TEST(LoopbackFixtures, httpRoutedGetReturnsCannedResponse) {
    LoopbackHttpServer server;
    server.route("/hello", 200, "hello, world", "text/plain");

    int32_t fd = connectClient(server.port());
    ASSERT_GE(fd, 0);

    std::string req =
        "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((int64_t) req.size(),
              __cajeta_net_send(fd, req.data(), (int64_t) req.size(), 0));

    std::string resp = recvToEof(fd);
    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos) << resp;
    EXPECT_NE(resp.find("Content-Length: 12"), std::string::npos) << resp;
    EXPECT_NE(resp.find("\r\n\r\nhello, world"), std::string::npos) << resp;
    EXPECT_EQ(server.hitCount("/hello"), 1);

    __cajeta_net_close(fd);
}

// --- LoopbackHttpServer: an unregistered path is a 404 ---------------------
TEST(LoopbackFixtures, httpUnknownPathIs404) {
    LoopbackHttpServer server;   // no routes registered

    int32_t fd = connectClient(server.port());
    ASSERT_GE(fd, 0);
    std::string req =
        "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((int64_t) req.size(),
              __cajeta_net_send(fd, req.data(), (int64_t) req.size(), 0));

    std::string resp = recvToEof(fd);
    EXPECT_NE(resp.find("HTTP/1.1 404 Not Found"), std::string::npos) << resp;
    EXPECT_EQ(server.hitCount("/nope"), 1);

    __cajeta_net_close(fd);
}

// --- LoopbackHttpServer: a POST body is drained + captured -----------------
// The upload-capture contract NET-8's request-body tests assert against:
// POST with a Content-Length body, and the fixture both completes the read
// and exposes the captured bytes via lastBody().
TEST(LoopbackFixtures, httpPostBodyCaptured) {
    LoopbackHttpServer server;
    server.route("/upload", 201, "created", "text/plain");

    int32_t fd = connectClient(server.port());
    ASSERT_GE(fd, 0);

    const std::string payload = "{\"k\":\"v\",\"n\":42}";
    std::ostringstream req;
    req << "POST /upload HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "Content-Length: " << payload.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << payload;
    std::string reqStr = req.str();
    ASSERT_EQ((int64_t) reqStr.size(),
              __cajeta_net_send(fd, reqStr.data(), (int64_t) reqStr.size(), 0));

    std::string resp = recvToEof(fd);
    EXPECT_NE(resp.find("HTTP/1.1 201 Created"), std::string::npos) << resp;

    EXPECT_EQ(server.lastBody("/upload"), payload)
        << "POST body was not captured byte-exact";
    EXPECT_EQ(server.hitCount("/upload"), 1);

    __cajeta_net_close(fd);
}

// --- LoopbackHttpServer: address() / baseUrl() report the bound port -------
TEST(LoopbackFixtures, httpServerReportsItsBoundEndpoint) {
    LoopbackHttpServer server;
    std::string expectAddr = "127.0.0.1:" + std::to_string(server.port());
    EXPECT_EQ(server.address(), expectAddr);
    EXPECT_EQ(server.baseUrl(), "http://" + expectAddr);
    EXPECT_NE(server.port(), 0);
}
