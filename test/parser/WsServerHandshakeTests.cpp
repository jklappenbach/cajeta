// NET-10.2 — cajeta.io.net.ws server-side WebSocket opening handshake
// (RFC 6455 §4.2). The server handshake is pure logic — an HttpRequest in,
// a 101 HttpResponse (or a HandshakeRejectedException) out, no sockets — so
// it tests directly over the JIT exactly the way WsFrameCodecTests /
// HttpServerTests drive the surrounding codecs: each test compiles a small
// Cajeta `run()` that builds a request, runs the handshake, and returns an
// int32 sentinel (1 on success, a distinct negative per failed sub-check).
//
// Pins the NET-10.2 deliverable: "Handshake — server: validate the client
// Upgrade request, compute + return `Sec-WebSocket-Accept`, switch the
// connection to WS framing." (plan/cajeta-net-plan.md, Phase 10) and its
// acceptance row acceptKeyMatchesRfcExample — the RFC 6455 §1.3 worked
// example: key "dGhlIHNhbXBsZSBub25jZQ==" -> accept
// "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.io.net.Headers;\n"
        "import cajeta.io.net.http.HttpRequest;\n"
        "import cajeta.io.net.http.HttpResponse;\n"
        "import cajeta.io.net.ws.WsServerHandshake;\n"
        "import cajeta.io.net.ws.HandshakeRejectedException;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// A well-formed RFC 6455 §4.2.1 Upgrade GET request, built in Cajeta. The
// key is the §1.3 worked-example nonce by default.
const char* validUpgradeRequest =
    "HttpRequest req = HttpRequest.get(\"/chat\");\n"
    "req.setHeader(\"Host\", \"server.example.com\");\n"
    "req.setHeader(\"Upgrade\", \"websocket\");\n"
    "req.setHeader(\"Connection\", \"Upgrade\");\n"
    "req.setHeader(\"Sec-WebSocket-Key\", \"dGhlIHNhbXBsZSBub25jZQ==\");\n"
    "req.setHeader(\"Sec-WebSocket-Version\", \"13\");\n";

} // namespace

// --- the acceptance vector: RFC 6455 §1.3 worked example ---------------
//
// acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
TEST(WsServerHandshakeTests, acceptKeyMatchesRfcExample) {
    EXPECT_EQ(runI32(
        "String accept = WsServerHandshake.acceptKey(\"dGhlIHNhbXBsZSBub25jZQ==\");\n"
        "String expected = \"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\";\n"
        "if (!accept.equals(expected)) return -1;\n"
        "return 1;"), 1);
}

// --- a valid Upgrade request produces a correct 101 response -----------
TEST(WsServerHandshakeTests, acceptBuildsSwitchingProtocolsResponse) {
    EXPECT_EQ(runI32(
        std::string(validUpgradeRequest) +
        "HttpResponse resp = WsServerHandshake.accept(req);\n"
        "if (resp.statusCode() != 101) return -1;\n"
        "Headers h = resp.getHeaders();\n"
        "String up = h.get(\"Upgrade\");\n"
        "if (up == null) return -2;\n"
        "if (!up.toLowerCase().equals(\"websocket\")) return -3;\n"
        "String conn = h.get(\"Connection\");\n"
        "if (conn == null) return -4;\n"
        "if (!conn.toLowerCase().equals(\"upgrade\")) return -5;\n"
        "String accept = h.get(\"Sec-WebSocket-Accept\");\n"
        "if (accept == null) return -6;\n"
        "if (!accept.equals(\"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\")) return -7;\n"
        "return 1;"), 1);
}

// --- isUpgradeRequest accepts the valid request ------------------------
TEST(WsServerHandshakeTests, isUpgradeRequestAcceptsValid) {
    EXPECT_EQ(runI32(
        std::string(validUpgradeRequest) +
        "if (!WsServerHandshake.isUpgradeRequest(req)) return -1;\n"
        "return 1;"), 1);
}

// --- Connection header with a token list (keep-alive, Upgrade) ---------
//
// The Connection header MUST be matched as a case-insensitive token list,
// not an exact value — a browser sends "keep-alive, Upgrade".
TEST(WsServerHandshakeTests, connectionTokenListMatchesUpgrade) {
    EXPECT_EQ(runI32(
        "HttpRequest req = HttpRequest.get(\"/chat\");\n"
        "req.setHeader(\"Upgrade\", \"WebSocket\");\n"        // mixed case
        "req.setHeader(\"Connection\", \"keep-alive, Upgrade\");\n"
        "req.setHeader(\"Sec-WebSocket-Key\", \"dGhlIHNhbXBsZSBub25jZQ==\");\n"
        "req.setHeader(\"Sec-WebSocket-Version\", \"13\");\n"
        "if (!WsServerHandshake.isUpgradeRequest(req)) return -1;\n"
        "HttpResponse resp = WsServerHandshake.accept(req);\n"
        "if (resp.statusCode() != 101) return -2;\n"
        "return 1;"), 1);
}

// --- a non-GET method is rejected with 400 -----------------------------
TEST(WsServerHandshakeTests, nonGetMethodRejected) {
    EXPECT_EQ(runI32(
        "HttpRequest req = HttpRequest.post(\"/chat\");\n"
        "req.setHeader(\"Upgrade\", \"websocket\");\n"
        "req.setHeader(\"Connection\", \"Upgrade\");\n"
        "req.setHeader(\"Sec-WebSocket-Key\", \"dGhlIHNhbXBsZSBub25jZQ==\");\n"
        "req.setHeader(\"Sec-WebSocket-Version\", \"13\");\n"
        "if (WsServerHandshake.isUpgradeRequest(req)) return -1;\n"
        "try {\n"
        "    WsServerHandshake.accept(req);\n"
        "    return -2;\n"
        "} catch (HandshakeRejectedException e) {\n"
        "    if (e.httpStatus != 400) return -3;\n"
        "    return 1;\n"
        "}"), 1);
}

// --- a missing Sec-WebSocket-Key is rejected with 400 ------------------
TEST(WsServerHandshakeTests, missingKeyRejected) {
    EXPECT_EQ(runI32(
        "HttpRequest req = HttpRequest.get(\"/chat\");\n"
        "req.setHeader(\"Upgrade\", \"websocket\");\n"
        "req.setHeader(\"Connection\", \"Upgrade\");\n"
        "req.setHeader(\"Sec-WebSocket-Version\", \"13\");\n"   // no key
        "try {\n"
        "    WsServerHandshake.accept(req);\n"
        "    return -1;\n"
        "} catch (HandshakeRejectedException e) {\n"
        "    if (e.httpStatus != 400) return -2;\n"
        "    return 1;\n"
        "}"), 1);
}

// --- a missing Upgrade header is rejected with 400 ---------------------
TEST(WsServerHandshakeTests, missingUpgradeHeaderRejected) {
    EXPECT_EQ(runI32(
        "HttpRequest req = HttpRequest.get(\"/chat\");\n"
        "req.setHeader(\"Connection\", \"Upgrade\");\n"        // no Upgrade
        "req.setHeader(\"Sec-WebSocket-Key\", \"dGhlIHNhbXBsZSBub25jZQ==\");\n"
        "req.setHeader(\"Sec-WebSocket-Version\", \"13\");\n"
        "try {\n"
        "    WsServerHandshake.accept(req);\n"
        "    return -1;\n"
        "} catch (HandshakeRejectedException e) {\n"
        "    if (e.httpStatus != 400) return -2;\n"
        "    return 1;\n"
        "}"), 1);
}

// --- a wrong Sec-WebSocket-Version is rejected with 426 ----------------
//
// RFC 6455 §4.4: a version other than 13 is "426 Upgrade Required", and
// the reject response echoes Sec-WebSocket-Version: 13.
TEST(WsServerHandshakeTests, wrongVersionRejectedWith426) {
    EXPECT_EQ(runI32(
        "HttpRequest req = HttpRequest.get(\"/chat\");\n"
        "req.setHeader(\"Upgrade\", \"websocket\");\n"
        "req.setHeader(\"Connection\", \"Upgrade\");\n"
        "req.setHeader(\"Sec-WebSocket-Key\", \"dGhlIHNhbXBsZSBub25jZQ==\");\n"
        "req.setHeader(\"Sec-WebSocket-Version\", \"8\");\n"   // not 13
        "if (WsServerHandshake.isUpgradeRequest(req)) return -1;\n"
        "try {\n"
        "    WsServerHandshake.accept(req);\n"
        "    return -2;\n"
        "} catch (HandshakeRejectedException e) {\n"
        "    if (e.httpStatus != 426) return -3;\n"
        "    HttpResponse rej = WsServerHandshake.reject(e);\n"
        "    if (rej.statusCode() != 426) return -4;\n"
        "    String ver = rej.getHeaders().get(\"Sec-WebSocket-Version\");\n"
        "    if (ver == null) return -5;\n"
        "    if (!ver.equals(\"13\")) return -6;\n"
        "    return 1;\n"
        "}"), 1);
}

// --- a different key yields a different (correct) accept ---------------
//
// A second independent RFC-style vector to guard against a hard-coded
// accept: key "x3JJHMbDL1EzLkh9GBhXDw==" -> "HSmrc0sMlYUkAGmm5OPpG2HaGWk="
// (the canonical example from many RFC 6455 implementations / Wikipedia).
TEST(WsServerHandshakeTests, secondKeyVectorAccept) {
    EXPECT_EQ(runI32(
        "String accept = WsServerHandshake.acceptKey(\"x3JJHMbDL1EzLkh9GBhXDw==\");\n"
        "if (!accept.equals(\"HSmrc0sMlYUkAGmm5OPpG2HaGWk=\")) return -1;\n"
        "return 1;"), 1);
}
