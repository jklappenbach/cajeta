// NET-9.1 — cajeta.net.http.HttpServer tests (HTTP/1.1 server core).
//
// The HttpServer connection loop is composed of pure codec layers — the
// incremental parser (NET-7.3), the body reader (NET-7.4), the serializer
// (NET-7.5), and the keep-alive policy (NET-7.6). Its *protocol* core —
// parse a request, read its body, dispatch to a handler, build the
// response, decide keep-alive, and map a malformed/oversized request to a
// status response — is therefore exercisable WITHOUT a live socket, over
// in-memory byte buffers, exactly the feed-then-drain golden-vector
// discipline HttpParserTests / BodyReader / HttpKeepAliveTests use.
//
// HttpServer.handleRequest(limits, handler, requestBytes, length) is that
// pure path: it runs the same parser -> body-reader -> handler ->
// keep-alive chain the live serveLoop does, returning an Exchange (the
// response, with its Connection header stamped, plus the reuse decision).
// Each test compiles a small Cajeta run() that builds a request, defines a
// handler lambda, asks HttpServer.handleRequest, and returns an int32
// sentinel (1 on success, a distinct negative per failed sub-check).
//
// The socket-facing loop (serveConnection/serveLoop over a live TcpStream,
// and the round-trip acceptance rows echoPostRoundTrips /
// bothModelsServeSameHandler / keepAliveServesTwoRequests) needs the fiber
// scheduler + reactor + loopback sockets — the same live-scheduler harness
// the NET-4 `ServerTests.*` JIT suite awaits — so it is out of scope for
// this pure unit suite (mirrors ServerLifecycleHarnessTests, which pins the
// NET-4.1 core one level down rather than over a live accept loop).
//
// Pins NET-9.1: "HttpServer over Server: read + parse requests, dispatch to
// a handler, write responses; keep-alive connection looping; per-request
// error -> response mapping." (plan/cajeta-net-plan.md, Phase 9).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the server, the message types,
// the parser limits, and String. The body must `return` int32.
//
// Helpers available to the body:
//   M.bytes(String s)      -> #int8[]   (a String's bytes as an array)
//   M.echo                 a (HttpRequest)->HttpResponse handler that
//                          echoes the request body back in a 200 with a
//                          Content-Type, defined as a local in run() per
//                          test (lambdas are locals in Cajeta).
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.http.HttpServer;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.net.http.HttpParserLimits;\n"
        "import cajeta.net.http.Exchange;\n"
        "import cajeta.net.NetException;\n"
        "public final class M {\n"
        "    // A String's UTF-8 bytes copied into a fresh owned array.\n"
        "    static #int8[] bytes(String s) {\n"
        "        int32 n = s.byteLength;\n"
        "        int8[] out = heap int8[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { out[i] = s.byteAt(i); i = i + 1; }\n"
        "        return #out;\n"
        "    }\n"
        "    // Run a request through the server's pure byte path.\n"
        "    static #Exchange handle((HttpRequest) -> #HttpResponse h, String req) {\n"
        "        int8[] rb = M.bytes(req);\n"
        "        return HttpServer.handleRequest(heap HttpParserLimits(), h, rb, rb.count());\n"
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

// --- a simple GET dispatches to the handler and returns its response ----

// THE basic dispatch: a GET with no body reaches the handler, whose
// response status + reason flow back out.
TEST(HttpServerTests, getDispatchesToHandler) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        "Exchange ex = M.handle(h, \"GET /hello HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n\");\n"
        "HttpResponse resp = ex.getResponse();\n"
        "if (resp.statusCode() != 200) return -1;\n"
        // The handler saw the request target.
        "return 1;"), 1);
}

// The handler can read the request fields (method + target) it was given.
TEST(HttpServerTests, handlerSeesRequestMethodAndTarget) {
    EXPECT_EQ(runI32(
        // Echo a 200 whose reason is "POST" iff the method was POST and the
        // target was /submit, else 400.
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    if (req.getMethod().equals(\"POST\") && req.getTarget().equals(\"/submit\")) {\n"
        "        return HttpResponse.of(200);\n"
        "    }\n"
        "    return HttpResponse.of(400);\n"
        "};\n"
        "Exchange ex = M.handle(h, \"POST /submit HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 0\\r\\n\\r\\n\");\n"
        "if (ex.getResponse().statusCode() != 200) return -1;\n"
        "return 1;"), 1);
}

// --- echo: a POST body round-trips through the handler ------------------

// The acceptance-shaped echo (over the pure byte path): a POST body is read
// per Content-Length, handed to the handler, and echoed back verbatim in
// the response body.
TEST(HttpServerTests, echoPostBodyRoundTrips) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    HttpResponse r = HttpResponse.of(200);\n"
        "    r.body(req.body, req.bodyLength());\n"
        "    return #r;\n"
        "};\n"
        "Exchange ex = M.handle(h, \"POST /echo HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 5\\r\\n\\r\\nhello\");\n"
        "HttpResponse resp = ex.getResponse();\n"
        "if (resp.statusCode() != 200) return -1;\n"
        "if (!resp.hasBody) return -2;\n"
        "if (resp.bodyLength() != 5) return -3;\n"
        // Body bytes are exactly 'hello'.
        "if (resp.body[0] != (int8) 104) return -4;\n"   // 'h'
        "if (resp.body[1] != (int8) 101) return -5;\n"   // 'e'
        "if (resp.body[2] != (int8) 108) return -6;\n"   // 'l'
        "if (resp.body[3] != (int8) 108) return -7;\n"   // 'l'
        "if (resp.body[4] != (int8) 111) return -8;\n"   // 'o'
        "return 1;"), 1);
}

// A chunked request body is transfer-decoded before the handler sees it.
TEST(HttpServerTests, chunkedRequestBodyIsDecodedForHandler) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    HttpResponse r = HttpResponse.of(200);\n"
        "    r.body(req.body, req.bodyLength());\n"
        "    return #r;\n"
        "};\n"
        // "Wikipedia" framed as two chunks: 4 ('Wiki') + 5 ('pedia') + 0.
        "Exchange ex = M.handle(h, \"POST /c HTTP/1.1\\r\\nHost: x\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n4\\r\\nWiki\\r\\n5\\r\\npedia\\r\\n0\\r\\n\\r\\n\");\n"
        "HttpResponse resp = ex.getResponse();\n"
        "if (resp.statusCode() != 200) return -1;\n"
        "if (resp.bodyLength() != 9) return -2;\n"        // 'Wikipedia'
        "if (resp.body[0] != (int8) 87) return -3;\n"     // 'W'
        "if (resp.body[8] != (int8) 97) return -4;\n"     // 'a'
        "return 1;"), 1);
}

// --- keep-alive decision -------------------------------------------------

// HTTP/1.1 default: no Connection header anywhere -> the exchange permits
// reuse, and no redundant Connection header is stamped on the response.
TEST(HttpServerTests, http11DefaultsToKeepAlive) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        "Exchange ex = M.handle(h, \"GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n\");\n"
        "if (!ex.isKeepAlive()) return -1;\n"
        // 1.1 + reuse -> the Connection header is left off (version default).
        "if (ex.getResponse().getHeaders().contains(\"Connection\")) return -2;\n"
        "return 1;"), 1);
}

// A request asking Connection: close ends reuse, and the response is
// stamped Connection: close so the peer sees the intent.
TEST(HttpServerTests, requestCloseEndsReuseAndStampsClose) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        "Exchange ex = M.handle(h, \"GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n\");\n"
        "if (ex.isKeepAlive()) return -1;\n"
        "HttpResponse resp = ex.getResponse();\n"
        "if (!resp.getHeaders().contains(\"Connection\")) return -2;\n"
        "String c = resp.getHeaders().get(\"Connection\");\n"
        "if (!c.equals(\"close\")) return -3;\n"
        "return 1;"), 1);
}

// HTTP/1.0 without keep-alive defaults to close (no reuse).
TEST(HttpServerTests, http10DefaultsToClose) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    HttpResponse r = HttpResponse.of(200);\n"
        "    r.version(\"HTTP/1.0\");\n"
        "    return #r;\n"
        "};\n"
        "Exchange ex = M.handle(h, \"GET / HTTP/1.0\\r\\nHost: x\\r\\n\\r\\n\");\n"
        "if (ex.isKeepAlive()) return -1;\n"
        "return 1;"), 1);
}

// --- error -> response mapping ------------------------------------------

// A malformed request line maps to 400 Bad Request, and the connection is
// closed (no reuse) since the byte stream is no longer reliably framed.
TEST(HttpServerTests, malformedRequestMapsTo400AndCloses) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        // A request line with only one token (no target/version) is malformed.
        "Exchange ex = M.handle(h, \"GET\\r\\nHost: x\\r\\n\\r\\n\");\n"
        "if (ex.getResponse().statusCode() != 400) return -1;\n"
        "if (ex.isKeepAlive()) return -2;\n"
        "return 1;"), 1);
}

// A header flood (over the configured maxHeaderCount) maps to 431 Request
// Header Fields Too Large via the virtual HttpException.httpStatus().
TEST(HttpServerTests, headerFloodMapsTo431) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        "HttpParserLimits lim = heap HttpParserLimits();\n"
        "lim.maxHeaderCount = 2;\n"
        // Four header lines exceed maxHeaderCount = 2.
        "String req = \"GET / HTTP/1.1\\r\\nA: 1\\r\\nB: 2\\r\\nC: 3\\r\\nD: 4\\r\\n\\r\\n\";\n"
        "int8[] rb = M.bytes(req);\n"
        "Exchange ex = HttpServer.handleRequest(lim, h, rb, rb.count());\n"
        "if (ex.getResponse().statusCode() != 431) return -1;\n"
        "if (ex.isKeepAlive()) return -2;\n"
        "return 1;"), 1);
}

// A handler that throws is isolated to a 500 Internal Server Error; the
// request itself parsed clean, so the connection may stay alive.
TEST(HttpServerTests, throwingHandlerMapsTo500) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    throw heap NetException(\"handler blew up\");\n"
        "};\n"
        "Exchange ex = M.handle(h, \"GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n\");\n"
        "if (ex.getResponse().statusCode() != 500) return -1;\n"
        // Request parsed clean (HTTP/1.1, no close) -> connection survives.
        "if (!ex.isKeepAlive()) return -2;\n"
        "return 1;"), 1);
}

// A truncated request (head started but never terminated) maps to 400 and
// closes (UnexpectedEof on endInput).
TEST(HttpServerTests, truncatedHeadMapsTo400) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> HttpResponse.of(200);\n"
        // No blank-line terminator -> the head never completes.
        "Exchange ex = M.handle(h, \"GET / HTTP/1.1\\r\\nHost: x\\r\\n\");\n"
        "if (ex.getResponse().statusCode() != 400) return -1;\n"
        "if (ex.isKeepAlive()) return -2;\n"
        "return 1;"), 1);
}

// --- full wire round-trip: handleRequestBytes serializes the response ---

// handleRequestBytes returns the response WIRE bytes — the exact path the
// live writer flushes. A 200 echo of "hi" produces a well-formed
// status-line + Content-Length head + body.
TEST(HttpServerTests, handleRequestBytesProducesWireResponse) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    HttpResponse r = HttpResponse.of(200);\n"
        "    r.body(req.body, req.bodyLength());\n"
        "    return #r;\n"
        "};\n"
        "int8[] rb = M.bytes(\"POST /e HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 2\\r\\n\\r\\nhi\");\n"
        "int8[] wire = HttpServer.handleRequestBytes(h, rb, rb.count());\n"
        "String s = heap String(#wire, wire.count());\n"
        // Status line first.
        "if (!s.startsWith(\"HTTP/1.1 200 OK\\r\\n\")) return -1;\n"
        // The auto Content-Length for the 2-byte echo body. HttpSerializer
        // emits the auto-supplied framing header lowercased (RFC 7230 §3.2
        // field names are case-insensitive; the golden wire vectors are
        // all-lowercase — see HttpSerializer.writeLengthHead).
        "if (s.indexOf(\"content-length: 2\") < 0) return -2;\n"
        // The body 'hi' is present after the head terminator.
        "if (s.indexOf(\"\\r\\n\\r\\nhi\") < 0) return -3;\n"
        "return 1;"), 1);
}
