// NET-7.3 — cajeta.net.http.HttpParser tests (incremental HTTP parser).
//
// The incremental parser is a resumable state machine over byte buffers
// — pure logic, no I/O — so it tests directly over the JIT: each test
// compiles a small Cajeta `run()` that feeds wire bytes to an
// HttpParser and returns an int32 sentinel (positive on success, a
// distinct negative per failed sub-check), the same convention
// HttpMessageTests / HttpHeadersTests / UriParseTests use.
//
// Wire bytes are written as Cajeta string literals with \r\n escapes
// (the Cajeta lexer decodes \r -> CR, \n -> LF, see
// LiteralExpression.cpp), then fed via `wire.bytes` / `wire.byteLength`.
// The vectors mirror test/net/golden/http/*.http byte-for-byte so this
// suite pins the same corpus the NET-13.2 golden meta-test validates.
//
// Pins the NET-7.3 deliverable: "Incremental request/response parser: a
// resumable state machine fed arbitrary byte chunks (handles a header
// split across reads), enforcing limits (max header size, max header
// count, max line length) to resist abuse. Emits a parsed head, then a
// body framing decision." (plan/cajeta-net-plan.md, Phase 7) and its
// acceptance rows splitFeedMatchesOneShot + oversizeHeadersRejected.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the parser, the message
// types, the header map, and String. The body must `return` int32.
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.Headers;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.net.http.HttpParser;\n"
        "import cajeta.net.http.HttpParserLimits;\n"
        "import cajeta.net.http.BodyFraming;\n"
        "import cajeta.net.http.HeadersTooLargeException;\n"
        "import cajeta.net.http.MalformedMessageException;\n"
        "import cajeta.net.http.UnexpectedEofException;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- request head parsing ----------------------------------------------

// A simple GET request (the request-get-simple.http corpus) parses into
// method/target/version + headers, and frames as no-body (KIND_NONE).
TEST(HttpParserTests, simpleGetRequestParses) {
    EXPECT_EQ(runI32(
        "String wire = \"GET /hello.txt HTTP/1.1\\r\\nHost: www.example.test\\r\\n"
        "User-Agent: cajeta-net/1.0\\r\\nAccept: */*\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "boolean done = p.feed(wire.bytes, wire.byteLength);\n"
        "if (!done) return -1;\n"
        "if (!p.isComplete()) return -2;\n"
        "HttpRequest r = p.getRequest();\n"
        "if (r == null) return -3;\n"
        "if (!r.getMethod().equals(\"GET\")) return -4;\n"
        "if (!r.getTarget().equals(\"/hello.txt\")) return -5;\n"
        "if (!r.getVersion().equals(\"HTTP/1.1\")) return -6;\n"
        "Headers h = r.getHeaders();\n"
        "if (!h.get(\"host\").equals(\"www.example.test\")) return -7;\n"
        "if (!h.get(\"accept\").equals(\"*/*\")) return -8;\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isNone()) return -9;\n"
        "if (!f.keepAlive) return -10;\n"
        "return 1;"), 1);
}

// A POST with Content-Length frames as KIND_CONTENT_LENGTH with the
// right length, and the body bytes that arrived after the head are
// captured as leftover (request-post-contentlength.http).
TEST(HttpParserTests, postContentLengthFramingAndLeftover) {
    EXPECT_EQ(runI32(
        "String wire = \"POST /submit HTTP/1.1\\r\\nHost: api.example.test\\r\\n"
        "Content-Type: application/json\\r\\nContent-Length: 17\\r\\n\\r\\n"
        "{\\\"name\\\":\\\"cajeta\\\"}\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "if (!p.isComplete()) return -1;\n"
        "HttpRequest r = p.getRequest();\n"
        "if (!r.getMethod().equals(\"POST\")) return -2;\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isContentLength()) return -3;\n"
        "if (f.length != (int64) 17) return -4;\n"
        // 17 body bytes arrived in the same chunk -> leftover.
        "if (p.leftoverLength() != 17) return -5;\n"
        "int8[] lo = p.leftover();\n"
        "if (lo[0] != (int8) 123) return -6;\n"   // '{'
        "return 1;"), 1);
}

// --- response head parsing ---------------------------------------------

// A 200 response with Content-Length parses status/reason + frames.
TEST(HttpParserTests, response200ContentLength) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\n"
        "Content-Length: 13\\r\\nConnection: keep-alive\\r\\n\\r\\nHello, world!\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "if (!p.isComplete()) return -1;\n"
        "HttpResponse r = p.getResponse();\n"
        "if (r == null) return -2;\n"
        "if (r.statusCode() != 200) return -3;\n"
        "if (!r.getReason().equals(\"OK\")) return -4;\n"
        "if (!r.getVersion().equals(\"HTTP/1.1\")) return -5;\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isContentLength()) return -6;\n"
        "if (f.length != (int64) 13) return -7;\n"
        "if (!f.keepAlive) return -8;\n"
        "if (p.leftoverLength() != 13) return -9;\n"
        "return 1;"), 1);
}

// A chunked response frames as KIND_CHUNKED (length -1), TE beats CL.
TEST(HttpParserTests, chunkedResponseFraming) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\n"
        "Transfer-Encoding: chunked\\r\\n\\r\\n4\\r\\nWiki\\r\\n0\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isChunked()) return -1;\n"
        "if (f.length != (int64) -1) return -2;\n"
        "return 1;"), 1);
}

// A 204 is definitionally bodyless even without framing headers, and
// Connection: close makes it non-keep-alive (response-204-close.http).
TEST(HttpParserTests, response204CloseIsBodylessNotKeepAlive) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.1 204 No Content\\r\\nConnection: close\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "HttpResponse r = p.getResponse();\n"
        "if (r.statusCode() != 204) return -1;\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isNone()) return -2;\n"
        "if (f.keepAlive) return -3;\n"
        "return 1;"), 1);
}

// A response with neither Content-Length nor chunked, not a bodyless
// status, is close-delimited (read to EOF) and not keep-alive.
TEST(HttpParserTests, responseNoFramingIsCloseDelimited) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\n\\r\\nbody\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isClose()) return -1;\n"
        "if (f.keepAlive) return -2;\n"
        "return 1;"), 1);
}

// A status line with no reason phrase parses (reason is empty).
TEST(HttpParserTests, statusLineEmptyReasonPhrase) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.1 200 \\r\\nContent-Length: 0\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "HttpResponse r = p.getResponse();\n"
        "if (r.statusCode() != 200) return -1;\n"
        "if (!r.getReason().equals(\"\")) return -2;\n"
        "return 1;"), 1);
}

// --- incrementality: split feed == one-shot ----------------------------

// THE keystone acceptance (splitFeedMatchesOneShot): a request split
// across three chunks — mid-request-line, mid-header, mid-body — parses
// identically to a single one-shot feed.
TEST(HttpParserTests, splitFeedMatchesOneShot) {
    EXPECT_EQ(runI32(
        "String w = \"POST /submit HTTP/1.1\\r\\nHost: api.example.test\\r\\n"
        "Content-Length: 5\\r\\n\\r\\nhello\";\n"
        "int8[] b = w.bytes;\n"
        "int32 n = w.byteLength;\n"
        // Feed byte-by-byte: the hardest split (every boundary exercised).
        "HttpParser p = HttpParser.forRequest();\n"
        "int32 i = 0;\n"
        "int8[] one = new int8[1];\n"
        "while (i < n) {\n"
        "    one[0] = b[i];\n"
        "    p.feed(one, 1);\n"
        "    i = i + 1;\n"
        "}\n"
        "if (!p.isComplete()) return -1;\n"
        "HttpRequest r = p.getRequest();\n"
        "if (!r.getMethod().equals(\"POST\")) return -2;\n"
        "if (!r.getTarget().equals(\"/submit\")) return -3;\n"
        "if (!r.getHeaders().get(\"host\").equals(\"api.example.test\")) return -4;\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isContentLength()) return -5;\n"
        "if (f.length != (int64) 5) return -6;\n"
        "if (p.leftoverLength() != 5) return -7;\n"
        "int8[] lo = p.leftover();\n"
        "if (lo[0] != (int8) 104) return -8;\n"   // 'h'
        "if (lo[4] != (int8) 111) return -9;\n"   // 'o'
        "return 1;"), 1);
}

// A header value split across two feeds (the CRLF lands in the second
// chunk) still parses correctly — the resume boundary is mid-line.
TEST(HttpParserTests, headerSplitAcrossFeeds) {
    EXPECT_EQ(runI32(
        "String a = \"GET / HTTP/1.1\\r\\nHost: exa\";\n"
        "String b = \"mple.test\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "boolean d1 = p.feed(a.bytes, a.byteLength);\n"
        "if (d1) return -1;\n"             // not complete after the first chunk
        "boolean d2 = p.feed(b.bytes, b.byteLength);\n"
        "if (!d2) return -2;\n"
        "if (!p.getRequest().getHeaders().get(\"host\").equals(\"example.test\")) return -3;\n"
        "return 1;"), 1);
}

// --- abuse resistance --------------------------------------------------

// oversizeHeadersRejected: a head over the maxHeadBytes budget raises
// HeadersTooLargeException before buffering unboundedly.
TEST(HttpParserTests, oversizeHeadersRejected) {
    EXPECT_EQ(runI32(
        // Tiny head budget so a normal request trips it.
        "HttpParserLimits lim = HttpParserLimits.of(32, 0, 0);\n"
        "String wire = \"GET / HTTP/1.1\\r\\nHost: a-really-long-header-value.example.test\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequestWithLimits(lim);\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"           // should have thrown
        "} catch (HeadersTooLargeException e) {\n"
        "    if (e.limit != (int64) 32) return -2;\n"
        "    return 1;\n"
        "}"), 1);
}

// A single over-long line trips maxLineBytes (long-header-line abuse).
TEST(HttpParserTests, overlongLineRejected) {
    EXPECT_EQ(runI32(
        "HttpParserLimits lim = HttpParserLimits.of(0, 16, 0);\n"
        "String wire = \"GET / HTTP/1.1\\r\\nX-Long: aaaaaaaaaaaaaaaaaaaaaaaaaaaa\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequestWithLimits(lim);\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (HeadersTooLargeException e) {\n"
        "    if (e.limit != (int64) 16) return -2;\n"
        "    return 1;\n"
        "}"), 1);
}

// Too many header fields trips maxHeaderCount (header-flood abuse).
TEST(HttpParserTests, tooManyHeadersRejected) {
    EXPECT_EQ(runI32(
        "HttpParserLimits lim = HttpParserLimits.of(0, 0, 2);\n"
        "String wire = \"GET / HTTP/1.1\\r\\nA: 1\\r\\nB: 2\\r\\nC: 3\\r\\nD: 4\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequestWithLimits(lim);\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (HeadersTooLargeException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- malformed + EOF ---------------------------------------------------

// endInput on an unterminated head raises UnexpectedEof
// (missing-crlf-terminator abuse).
TEST(HttpParserTests, unterminatedHeadEndInputRaisesEof) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nHost: h.test\\r\\nX-A: b\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "boolean done = p.feed(wire.bytes, wire.byteLength);\n"
        "if (done) return -1;\n"           // head not terminated -> not done
        "try {\n"
        "    p.endInput();\n"
        "    return -2;\n"                  // should have thrown
        "} catch (UnexpectedEofException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// A header line with no ':' is a MalformedMessage citing the offset.
TEST(HttpParserTests, headerLineWithoutColonIsMalformed) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nNotAHeaderLine\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (MalformedMessageException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// A non-numeric status code is a MalformedMessage.
TEST(HttpParserTests, nonNumericStatusCodeIsMalformed) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.1 2X0 OK\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (MalformedMessageException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// A non-numeric Content-Length is a MalformedMessage.
TEST(HttpParserTests, badContentLengthIsMalformed) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.1 200 OK\\r\\nContent-Length: abc\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (MalformedMessageException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// HTTP/1.0 defaults to NOT keep-alive (unless Connection: keep-alive).
TEST(HttpParserTests, http10DefaultsToClose) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.0 200 OK\\r\\nContent-Length: 0\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "BodyFraming f = p.getFraming();\n"
        "if (f.keepAlive) return -1;\n"
        "return 1;"), 1);
}

// HTTP/1.0 with explicit Connection: keep-alive IS keep-alive.
TEST(HttpParserTests, http10KeepAliveHonored) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.0 200 OK\\r\\nConnection: keep-alive\\r\\n"
        "Content-Length: 0\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.keepAlive) return -1;\n"
        "return 1;"), 1);
}
