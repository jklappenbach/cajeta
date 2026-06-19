//
// MockPeerTests.cpp — self-validation of the NET-13.3 scriptable mock
// peers (test/net/MockPeer.h).
//
// NET-13.3 ships *test infrastructure* (a scriptable misbehaving server)
// that the later client error-path phases (NET-8 HTTP client, NET-10
// WebSocket) drive against to make their failure handling deterministic.
// Those phases don't exist yet, so — exactly like the NET-13.1 fixture
// meta-test and the NET-13.2 golden-corpus meta-test — this suite
// validates the **fixture itself**: that each scripted wire condition
// (canned response, redirect chain across reconnections, slow drip,
// malformed framing, premature EOF mid-body, abort mid-headers) is
// replayed byte-faithfully and observably to a connected client.
//
// As in LoopbackFixturesTests.cpp, the *client* half of every round-trip
// is driven through the product's own NET-1.1 socket intrinsics
// (`__cajeta_net_socket` / `_connect` / `_send` / `_recv` / `_close`) —
// the same symbols `TcpStream` (NET-1.3) lowers to — so this both keeps
// the mock honest and demonstrates it is a valid peer for a cajeta.io.net
// client, which is its reason to exist.
//
#include "gtest/gtest.h"

#include "net/MockPeer.h"

#include <chrono>
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

// The product's NET-1.1 socket intrinsics — the *client* driver, resolved
// via extern "C" against the natively-linked runtime (cajeta_runtime.c
// #includes cajeta_net_socket.c), the same way NetSocketTests and
// LoopbackFixturesTests reach them.
extern "C" {
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len, int32_t flags);
    int64_t __cajeta_net_recv(int32_t fd, void* buf, int64_t len, int32_t flags);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_last_error(void);
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

int32_t connectClient(uint16_t port) {
    int32_t fd = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0) << "socket err=" << __cajeta_net_last_error();
    sockaddr_in addr = loopbackV4(port);
    EXPECT_EQ(0, __cajeta_net_connect(fd, &addr, (int32_t) sizeof(addr)))
        << "connect err=" << __cajeta_net_last_error();
    return fd;
}

// Send a minimal GET so the mock's request-drain completes and replies.
void sendGet(int32_t fd, const std::string& path = "/") {
    std::string req = "GET " + path +
        " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    __cajeta_net_send(fd, req.data(), (int64_t) req.size(), 0);
}

// Read until the peer closes (orderly EOF) — the delimiter for the mock's
// closeWrite()/abort()-terminated scripts.
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

// --- a canned response is replayed byte-faithfully -------------------------
// The base contract: the script's bytes arrive at the client exactly, and
// the orderly closeWrite() terminates the read.
TEST(MockPeer, replaysCannedResponse) {
    MockPeer peer(MockScript().ok("hello, mock"));
    ASSERT_NE(peer.port(), 0);

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    sendGet(fd);

    std::string resp = recvToEof(fd);
    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos) << resp;
    EXPECT_NE(resp.find("Content-Length: 11"), std::string::npos) << resp;
    EXPECT_NE(resp.find("\r\n\r\nhello, mock"), std::string::npos) << resp;

    __cajeta_net_close(fd);
}

// --- the mock captures the client's request --------------------------------
// drainRequest (on by default) reads the request to the header terminator
// so a request-then-read client isn't deadlocked, and exposes it for an
// assertion that the client wrote what was expected.
TEST(MockPeer, capturesClientRequest) {
    MockPeer peer(MockScript().ok("x"));

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    sendGet(fd, "/the/path");
    (void) recvToEof(fd);
    __cajeta_net_close(fd);

    std::string req = peer.lastRequest();
    EXPECT_NE(req.find("GET /the/path HTTP/1.1"), std::string::npos) << req;
    EXPECT_NE(req.find("\r\n\r\n"), std::string::npos) << req;
}

// --- a redirect chain plays one script per reconnection --------------------
// Each hop answers Connection: close, so the client reconnects per hop; the
// Nth connection plays the Nth script. We assert the sequence of statuses a
// follow-redirect client would walk, terminating in the 200.
TEST(MockPeer, redirectChainAcrossReconnections) {
    MockPeer peer(std::vector<MockScript>{
        MockScript().redirect(302, "/b"),
        MockScript().redirect(307, "/c"),
        MockScript().ok("arrived"),
    });

    // Hop 1 — 302 to /b.
    {
        int32_t fd = connectClient(peer.port());
        ASSERT_GE(fd, 0);
        sendGet(fd, "/a");
        std::string r = recvToEof(fd);
        EXPECT_NE(r.find("HTTP/1.1 302 Found"), std::string::npos) << r;
        EXPECT_NE(r.find("Location: /b"), std::string::npos) << r;
        __cajeta_net_close(fd);
    }
    // Hop 2 — 307 to /c.
    {
        int32_t fd = connectClient(peer.port());
        ASSERT_GE(fd, 0);
        sendGet(fd, "/b");
        std::string r = recvToEof(fd);
        EXPECT_NE(r.find("HTTP/1.1 307 Temporary Redirect"),
                  std::string::npos) << r;
        EXPECT_NE(r.find("Location: /c"), std::string::npos) << r;
        __cajeta_net_close(fd);
    }
    // Hop 3 — 200 terminal.
    {
        int32_t fd = connectClient(peer.port());
        ASSERT_GE(fd, 0);
        sendGet(fd, "/c");
        std::string r = recvToEof(fd);
        EXPECT_NE(r.find("HTTP/1.1 200 OK"), std::string::npos) << r;
        EXPECT_NE(r.find("\r\n\r\narrived"), std::string::npos) << r;
        __cajeta_net_close(fd);
    }

    EXPECT_EQ(peer.acceptedConnections(), 3);
}

// --- a slow drip delivers the same bytes, paced -----------------------------
// The response is emitted in three sends with a delay between; the full
// payload still reassembles client-side, and the elapsed time reflects the
// scripted pacing (a read-timeout test would key off the gap). We assert the
// payload exactly and that at least the drip delay elapsed.
TEST(MockPeer, slowDripReassemblesWithPacing) {
    MockPeer peer(MockScript()
        .send("HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n")
        .delay(30).send("ab")
        .delay(30).send("cd")
        .delay(30).send("ef")
        .closeWrite());

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    sendGet(fd);

    auto t0 = std::chrono::steady_clock::now();
    std::string resp = recvToEof(fd);
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_NE(resp.find("\r\n\r\nabcdef"), std::string::npos) << resp;
    // Three 30ms gaps were scripted before the trailing chunks; allow slack
    // but require the pacing was observable (not delivered all at once).
    EXPECT_GE(elapsedMs, 50) << "drip pacing not observed (" << elapsedMs << "ms)";

    __cajeta_net_close(fd);
}

// --- malformed status line is delivered verbatim ---------------------------
// The mock does NOT sanitize: the bogus status line reaches the client so
// the parser's reject path is what's under test downstream. Here we assert
// the mock relayed the garbage rather than "fixing" it.
TEST(MockPeer, malformedStatusLineDeliveredVerbatim) {
    MockPeer peer(MockScript().malformedStatusLine());

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    sendGet(fd);
    std::string resp = recvToEof(fd);

    EXPECT_NE(resp.find("NOT-HTTP garbage line"), std::string::npos) << resp;
    EXPECT_EQ(resp.find("HTTP/1.1"), std::string::npos)
        << "mock must not inject a valid status line: " << resp;

    __cajeta_net_close(fd);
}

// --- a bad Content-Length value reaches the client -------------------------
TEST(MockPeer, badContentLengthDelivered) {
    MockPeer peer(MockScript().badContentLength("-7"));

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    sendGet(fd);
    std::string resp = recvToEof(fd);

    EXPECT_NE(resp.find("Content-Length: -7"), std::string::npos) << resp;

    __cajeta_net_close(fd);
}

// --- a non-hex chunk size reaches the client -------------------------------
TEST(MockPeer, badChunkSizeDelivered) {
    MockPeer peer(MockScript().badChunkSize("zz"));

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    sendGet(fd);
    std::string resp = recvToEof(fd);

    EXPECT_NE(resp.find("Transfer-Encoding: chunked"), std::string::npos) << resp;
    EXPECT_NE(resp.find("\r\n\r\nzz\r\n"), std::string::npos) << resp;

    __cajeta_net_close(fd);
}

// --- premature EOF mid-body: fewer bytes than Content-Length, then abort ----
// The truncation case the client's UnexpectedEofException path must catch:
// the headers promise 10 bytes but only 4 arrive before EOF.
TEST(MockPeer, prematureEofMidBody) {
    MockPeer peer(MockScript().truncatedBody(10, "frag"));

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    sendGet(fd);
    std::string resp = recvToEof(fd);

    EXPECT_NE(resp.find("Content-Length: 10"), std::string::npos) << resp;
    // The body delivered is the 4-byte fragment, not the promised 10.
    size_t bodyStart = resp.find("\r\n\r\n");
    ASSERT_NE(bodyStart, std::string::npos) << resp;
    std::string body = resp.substr(bodyStart + 4);
    EXPECT_EQ(body, "frag");
    EXPECT_LT(body.size(), 10u) << "expected a truncated body";

    __cajeta_net_close(fd);
}

// --- premature EOF mid-headers: abort before CRLFCRLF ----------------------
// The peer vanishes partway through the header block: the client never sees
// the header terminator before EOF.
TEST(MockPeer, prematureEofMidHeaders) {
    MockPeer peer(MockScript().truncatedHeaders());

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    sendGet(fd);
    std::string resp = recvToEof(fd);

    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos) << resp;
    EXPECT_EQ(resp.find("\r\n\r\n"), std::string::npos)
        << "headers must be truncated before the terminator: " << resp;

    __cajeta_net_close(fd);
}

// --- a valid chunked response round-trips -----------------------------------
// The well-formed counterpart to badChunkSize: a streaming chunked-decoder
// test's happy peer. We assert the wire framing the decoder consumes.
TEST(MockPeer, validChunkedResponse) {
    MockPeer peer(MockScript().chunked({"Wiki", "pedia"}));

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    sendGet(fd);
    std::string resp = recvToEof(fd);

    EXPECT_NE(resp.find("Transfer-Encoding: chunked"), std::string::npos) << resp;
    EXPECT_NE(resp.find("\r\n4\r\nWiki\r\n"), std::string::npos) << resp;
    EXPECT_NE(resp.find("\r\n5\r\npedia\r\n"), std::string::npos) << resp;
    EXPECT_NE(resp.find("\r\n0\r\n\r\n"), std::string::npos) << resp;

    __cajeta_net_close(fd);
}

// --- server-speaks-first: drainRequest(false) replies without a request -----
// For a protocol where the server greets first (or a test that wants to
// observe the reply before sending), disabling the request drain lets the
// script play immediately on accept.
TEST(MockPeer, serverSpeaksFirstWhenDrainDisabled) {
    MockPeer peer(MockScript().send("GREETING\n").closeWrite());
    peer.drainRequest(false);

    int32_t fd = connectClient(peer.port());
    ASSERT_GE(fd, 0);
    // No request sent — the mock greets first.
    std::string resp = recvToEof(fd);
    EXPECT_EQ(resp, "GREETING\n");

    __cajeta_net_close(fd);
}
