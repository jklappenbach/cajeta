// NET-9.6 — cajeta.net.http server limits + hardening tests.
//
// NET-9.6 adds four request ceilings to the HTTP/1.1 server (NET-9.1/9.4):
// a head-read deadline (slowloris mitigation), a body-read deadline, a max
// body size (413 Payload Too Large), and Expect: 100-continue handling
// (100 Continue / 417 Expectation Failed). The deadline halves are
// socket-bound (they ride AsyncReader.readWithin -> TcpStream.readWithin,
// covered by the NET-3.4 TimeoutDeregister suite + the live-scheduler
// harness), but the *policy* — the body-size cap and the expect-continue
// decision — is pure logic over the parsed request + ServerLimits, so it is
// exercisable WITHOUT a live socket over in-memory byte buffers, exactly the
// feed-then-drain golden-vector discipline HttpServerTests uses.
//
// Two pure entry points are pinned here:
//   * HttpServer.handleRequestWithLimits(parseLimits, serverLimits, handler,
//     bytes, len) -> Exchange — the hardened byte path: it enforces the body
//     size cap + expect short-circuits (413/417) before/while reading the
//     body, returning the Exchange (response + reuse decision).
//   * HttpServer.expectAction(parseLimits, serverLimits, bytes, len) -> the
//     ExpectContinue ACTION_* the server would take (PROCEED / SEND_CONTINUE
//     / EXPECTATION_FAILED / REJECT_TOO_LARGE), plus
//     HttpServer.continueResponse() -> the "HTTP/1.1 100 Continue\r\n\r\n"
//     wire bytes flushed before the body when SEND_CONTINUE is chosen.
//
// Pins NET-9.6: "Limits + hardening: request timeout, max body size,
// slowloris mitigation (header-read deadline), 100-continue handling."
// (plan/cajeta-net-plan.md, Phase 9) and the acceptance row
// HttpServerTests.slowlorisHeaderTimeout's policy half.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the server + message types + both
// limits types + the expect-continue policy. The body must `return` int32.
//
// Helpers available to the body:
//   M.bytes(String s)        -> #int8[]   (a String's bytes as an array)
//   M.handle(slim, h, req)   -> #Exchange (the hardened byte path with
//                               default parse limits + the given ServerLimits)
//   M.action(slim, req)      -> int32     (the ExpectContinue action)
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.http.HttpServer;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.net.http.HttpParserLimits;\n"
        "import cajeta.net.http.ServerLimits;\n"
        "import cajeta.net.http.ExpectContinue;\n"
        "import cajeta.net.http.Exchange;\n"
        "import cajeta.net.NetException;\n"
        "public final class M {\n"
        "    static #int8[] bytes(String s) {\n"
        "        int32 n = s.byteLength;\n"
        "        int8[] out = new int8[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { out[i] = s.byteAt(i); i = i + 1; }\n"
        "        return #out;\n"
        "    }\n"
        "    static #Exchange handle(ServerLimits slim,\n"
        "                            (HttpRequest) -> #HttpResponse h, String req) {\n"
        "        int8[] rb = M.bytes(req);\n"
        "        return HttpServer.handleRequestWithLimits(\n"
        "            heap HttpParserLimits(), slim, h, rb, rb.count());\n"
        "    }\n"
        "    static int32 action(ServerLimits slim, String req) {\n"
        "        int8[] rb = M.bytes(req);\n"
        "        return HttpServer.expectAction(heap HttpParserLimits(), slim,\n"
        "                                       rb, rb.count());\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- ServerLimits value type --------------------------------------------

// Defaults: a positive head/body deadline, a positive body cap, expect on.
TEST(HttpServerHardeningTests, serverLimitsDefaults) {
    EXPECT_EQ(runI32(
        "ServerLimits l = heap ServerLimits();\n"
        "if (!l.hasHeadDeadline()) return -1;\n"
        "if (!l.hasBodyDeadline()) return -2;\n"
        "if (!l.hasBodyCap()) return -3;\n"
        "if (!l.expectContinueEnabled) return -4;\n"
        // A 5-byte body is within the 16 MiB default cap.
        "if (!l.bodyWithinCap((int64) 5)) return -5;\n"
        "return 1;"), 1);
}

// of() takes non-positive values VERBATIM as "disabled" (unlike
// HttpParserLimits.of, which normalizes to defaults) — a hardening policy
// must be able to remove a ceiling deliberately.
TEST(HttpServerHardeningTests, serverLimitsOfDisablesOnZero) {
    EXPECT_EQ(runI32(
        "ServerLimits l = ServerLimits.of(0, 0, (int64) 0, false);\n"
        "if (l.hasHeadDeadline()) return -1;\n"
        "if (l.hasBodyDeadline()) return -2;\n"
        "if (l.hasBodyCap()) return -3;\n"
        "if (l.expectContinueEnabled) return -4;\n"
        // No cap -> any size is within.
        "if (!l.bodyWithinCap((int64) 999999999)) return -5;\n"
        "return 1;"), 1);
}

// bodyWithinCap is an inclusive <= comparison at the boundary.
TEST(HttpServerHardeningTests, bodyWithinCapBoundary) {
    EXPECT_EQ(runI32(
        "ServerLimits l = ServerLimits.of(0, 0, (int64) 10, true);\n"
        "if (!l.bodyWithinCap((int64) 10)) return -1;\n"   // == cap ok
        "if (l.bodyWithinCap((int64) 11)) return -2;\n"    // over -> not ok
        "return 1;"), 1);
}

// --- max body size (413) -------------------------------------------------

// A declared Content-Length over the cap is rejected up front with 413,
// without dispatching the handler, and the connection is closed.
TEST(HttpServerHardeningTests, contentLengthOverCapRejected413) {
    EXPECT_EQ(runI32(
        // Handler would return 200; it must NOT run (rejected up front).
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        "ServerLimits l = ServerLimits.of(0, 0, (int64) 4, true);\n"
        // Content-Length 5 > cap 4.
        "Exchange ex = M.handle(l, h, \"POST /u HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 5\\r\\n\\r\\nhello\");\n"
        "if (ex.getResponse().statusCode() != 413) return -1;\n"
        "if (ex.isKeepAlive()) return -2;\n"
        "return 1;"), 1);
}

// A body within the cap dispatches normally (the cap doesn't false-trip).
TEST(HttpServerHardeningTests, bodyWithinCapDispatches) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    HttpResponse r = HttpResponse.of(200);\n"
        "    r.body(req.body, req.bodyLength());\n"
        "    return #r;\n"
        "};\n"
        "ServerLimits l = ServerLimits.of(0, 0, (int64) 1024, true);\n"
        "Exchange ex = M.handle(l, h, \"POST /e HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 5\\r\\n\\r\\nhello\");\n"
        "HttpResponse resp = ex.getResponse();\n"
        "if (resp.statusCode() != 200) return -1;\n"
        "if (resp.bodyLength() != 5) return -2;\n"
        "return 1;"), 1);
}

// A chunked body whose DECODED total streams past the cap is rejected 413
// mid-stream (chunked length is unknown up front, so the running total is
// the guard).
TEST(HttpServerHardeningTests, chunkedOverCapRejected413) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        // Cap 5; 'Wikipedia' decodes to 9 bytes (4 + 5).
        "ServerLimits l = ServerLimits.of(0, 0, (int64) 5, true);\n"
        "Exchange ex = M.handle(l, h, \"POST /c HTTP/1.1\\r\\nHost: x\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n4\\r\\nWiki\\r\\n5\\r\\npedia\\r\\n0\\r\\n\\r\\n\");\n"
        "if (ex.getResponse().statusCode() != 413) return -1;\n"
        "if (ex.isKeepAlive()) return -2;\n"
        "return 1;"), 1);
}

// A chunked body within the cap decodes + dispatches normally.
TEST(HttpServerHardeningTests, chunkedWithinCapDispatches) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    HttpResponse r = HttpResponse.of(200);\n"
        "    r.body(req.body, req.bodyLength());\n"
        "    return #r;\n"
        "};\n"
        "ServerLimits l = ServerLimits.of(0, 0, (int64) 100, true);\n"
        "Exchange ex = M.handle(l, h, \"POST /c HTTP/1.1\\r\\nHost: x\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n4\\r\\nWiki\\r\\n5\\r\\npedia\\r\\n0\\r\\n\\r\\n\");\n"
        "if (ex.getResponse().statusCode() != 200) return -1;\n"
        "if (ex.getResponse().bodyLength() != 9) return -2;\n"
        "return 1;"), 1);
}

// --- Expect: 100-continue ------------------------------------------------

// A plain request (no Expect) proceeds.
TEST(HttpServerHardeningTests, noExpectProceeds) {
    EXPECT_EQ(runI32(
        "ServerLimits l = heap ServerLimits();\n"
        "int32 a = M.action(l, \"POST /u HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 3\\r\\n\\r\\nabc\");\n"
        "if (a != ExpectContinue.ACTION_PROCEED) return -1;\n"
        "return 1;"), 1);
}

// Expect: 100-continue on HTTP/1.1 within the cap -> SEND_CONTINUE.
TEST(HttpServerHardeningTests, expectContinueSendsContinue) {
    EXPECT_EQ(runI32(
        "ServerLimits l = heap ServerLimits();\n"
        "int32 a = M.action(l, \"POST /u HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 3\\r\\nExpect: 100-continue\\r\\n\\r\\n\");\n"
        "if (a != ExpectContinue.ACTION_SEND_CONTINUE) return -1;\n"
        "return 1;"), 1);
}

// The Expect token is matched case-insensitively.
TEST(HttpServerHardeningTests, expectContinueCaseInsensitive) {
    EXPECT_EQ(runI32(
        "ServerLimits l = heap ServerLimits();\n"
        "int32 a = M.action(l, \"POST /u HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 3\\r\\nExpect: 100-Continue\\r\\n\\r\\n\");\n"
        "if (a != ExpectContinue.ACTION_SEND_CONTINUE) return -1;\n"
        "return 1;"), 1);
}

// Expect: 100-continue but the declared Content-Length exceeds the cap ->
// REJECT_TOO_LARGE (413 without the 100, so the client never sends the body).
TEST(HttpServerHardeningTests, expectContinueOverCapRejects) {
    EXPECT_EQ(runI32(
        "ServerLimits l = ServerLimits.of(0, 0, (int64) 4, true);\n"
        "int32 a = M.action(l, \"POST /u HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 5\\r\\nExpect: 100-continue\\r\\n\\r\\n\");\n"
        "if (a != ExpectContinue.ACTION_REJECT_TOO_LARGE) return -1;\n"
        // ...and the hardened byte path turns that into a 413, closed.
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        "Exchange ex = M.handle(l, h, \"POST /u HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 5\\r\\nExpect: 100-continue\\r\\n\\r\\n\");\n"
        "if (ex.getResponse().statusCode() != 413) return -2;\n"
        "if (ex.isKeepAlive()) return -3;\n"
        "return 1;"), 1);
}

// An UNSUPPORTED expectation (any non-100-continue token) -> 417, closed.
TEST(HttpServerHardeningTests, unsupportedExpectFails417) {
    EXPECT_EQ(runI32(
        "ServerLimits l = heap ServerLimits();\n"
        "int32 a = M.action(l, \"POST /u HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 0\\r\\nExpect: weird-thing\\r\\n\\r\\n\");\n"
        "if (a != ExpectContinue.ACTION_EXPECTATION_FAILED) return -1;\n"
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        "Exchange ex = M.handle(l, h, \"POST /u HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 0\\r\\nExpect: weird-thing\\r\\n\\r\\n\");\n"
        "HttpResponse resp = ex.getResponse();\n"
        "if (resp.statusCode() != 417) return -2;\n"
        // 417's reason phrase is filled (outside HttpResponse.reasonFor's table).
        "if (!resp.getReason().equals(\"Expectation Failed\")) return -3;\n"
        "if (ex.isKeepAlive()) return -4;\n"
        "return 1;"), 1);
}

// When the server opts OUT of expect-continue, a 100-continue request just
// proceeds (no interim, no 417) — the client falls back to its own timeout.
TEST(HttpServerHardeningTests, expectDisabledProceeds) {
    EXPECT_EQ(runI32(
        "ServerLimits l = ServerLimits.of(0, 0, (int64) 0, false);\n"
        "int32 a = M.action(l, \"POST /u HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 3\\r\\nExpect: 100-continue\\r\\n\\r\\n\");\n"
        "if (a != ExpectContinue.ACTION_PROCEED) return -1;\n"
        "return 1;"), 1);
}

// Expect: 100-continue on HTTP/1.0 proceeds (1xx interim is 1.1-only).
TEST(HttpServerHardeningTests, expectContinueHttp10Proceeds) {
    EXPECT_EQ(runI32(
        "ServerLimits l = heap ServerLimits();\n"
        "int32 a = M.action(l, \"POST /u HTTP/1.0\\r\\nHost: x\\r\\nContent-Length: 3\\r\\nExpect: 100-continue\\r\\n\\r\\n\");\n"
        "if (a != ExpectContinue.ACTION_PROCEED) return -1;\n"
        "return 1;"), 1);
}

// The interim 100 Continue wire bytes are exactly the bare status line + the
// terminating blank line.
TEST(HttpServerHardeningTests, continueResponseWireBytes) {
    EXPECT_EQ(runI32(
        "int8[] wire = HttpServer.continueResponse();\n"
        "String s = heap String(#wire, wire.count());\n"
        "if (!s.equals(\"HTTP/1.1 100 Continue\\r\\n\\r\\n\")) return -1;\n"
        "return 1;"), 1);
}
