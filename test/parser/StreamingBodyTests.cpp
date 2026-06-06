// NET-9.4 — streaming responses + requests (server side).
//
// NET-9.4 lets a handler write a chunked response body INCREMENTALLY
// (without holding the whole payload) and read a streaming request body
// (a large upload, without buffering it). The genuinely new logic is two
// pure/seam-testable pieces, exercised here over the JIT exactly as the
// NET-7.4 BodyReader / NET-7.5 HttpSerializer suites are — no live socket:
//
//   * ChunkedEncoder (pure codec, golden-testable): frames one body chunk
//     as <hex-size> CRLF <data> CRLF, and the last-chunk terminator
//     0 CRLF CRLF. The exact encode-side mirror of the chunked BodyReader,
//     so encode -> decode round-trips byte-for-byte against the same
//     chunked golden corpus (test/net/golden/http/chunked.vectors).
//
//   * HttpSerializer.responseChunkedHead: the status line + headers
//     (Transfer-Encoding: chunked) + blank line a streaming response
//     writes ONCE before pushing chunks — no body, no terminator.
//
//   * RequestBodyStream: the streaming request-body reader. Drives a
//     BodyReader over an AsyncReader, yielding decoded body slices
//     incrementally. Exercised over the AsyncReader stage()/markEof()
//     test seam (no live socket), the same feed-then-drain discipline the
//     BodyReader golden tests use.
//
// The socket-facing ResponseBodyWriter (driving a live AsyncWriter flush)
// needs the reactor + a loopback socket, so — like HttpServer's
// serveConnection — it is out of scope for this pure unit suite; the chunk
// framing it sequences IS pinned here via ChunkedEncoder.
//
// Pins NET-9.4: "Streaming responses + requests: handler can write a
// chunked body incrementally and read a streaming request body (large
// uploads) without buffering." (plan/cajeta-net-plan.md, Phase 9).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the streaming primitives, the
// chunked decoder (for round-trip witnesses), the framing type, the
// message types, the serializer, the buffered reader, and a TcpStream the
// AsyncReader borrows but — driven only through stage()/markEof() — never
// actually touches a socket on. The body must `return` int32.
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.AsyncReader;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.net.http.HttpSerializer;\n"
        "import cajeta.net.http.ChunkedEncoder;\n"
        "import cajeta.net.http.BodyReader;\n"
        "import cajeta.net.http.BodyFraming;\n"
        "import cajeta.net.http.RequestBodyStream;\n"
        "import cajeta.net.http.UnexpectedEofException;\n"
        "public final class M {\n"
        "    // A String's UTF-8 bytes copied into a fresh owned array.\n"
        "    static #int8[] bytes(String s) {\n"
        "        int32 n = s.byteLength;\n"
        "        int8[] out = heap int8[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { out[i] = s.byteAt(i); i = i + 1; }\n"
        "        return #out;\n"
        "    }\n"
        "    // An AsyncReader pre-staged with `s` and marked EOF — the\n"
        "    // no-socket seam (TcpStream is borrowed but never read).\n"
        "    static #AsyncReader staged(String s) {\n"
        "        AsyncReader r = heap AsyncReader((TcpStream) null);\n"
        "        int8[] b = M.bytes(s);\n"
        "        r.stage(b, b.count());\n"
        "        r.markEof();\n"
        "        return #r;\n"
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

// ===== ChunkedEncoder — pure chunk framing ==============================

// One chunk frames as <hex-size> CRLF <data> CRLF. "hello" (5) -> the
// golden single-chunk prefix "5\r\nhello\r\n" (cf. chunked.vectors
// `single`: 350d0a68656c6c6f0d0a...).
TEST(StreamingBodyTests, encodeChunkFramesSingleChunk) {
    EXPECT_EQ(runI32(
        "int8[] data = M.bytes(\"hello\");\n"
        "int8[] c = ChunkedEncoder.encodeChunk(data, 5);\n"
        "String s = heap String(#c, c.count());\n"
        "if (!s.equals(\"5\\r\\nhello\\r\\n\")) return -1;\n"
        "return 1;"), 1);
}

// A chunk whose size needs two hex digits renders lowercase, no leading
// zero: 26 bytes -> "1a".
TEST(StreamingBodyTests, encodeChunkSizeIsLowercaseHexNoLeadingZero) {
    EXPECT_EQ(runI32(
        "int8[] data = M.bytes(\"abcdefghijklmnopqrstuvwxyz\");\n"   // 26 bytes
        "int8[] c = ChunkedEncoder.encodeChunk(data, 26);\n"
        "String s = heap String(#c, c.count());\n"
        "if (!s.startsWith(\"1a\\r\\n\")) return -1;\n"
        "if (!s.endsWith(\"\\r\\n\")) return -2;\n"
        // Total = 2 (size) + 2 (CRLF) + 26 (data) + 2 (CRLF) = 32.
        "if (c.count() != 32) return -3;\n"
        "return 1;"), 1);
}

// A zero-length piece must NOT frame as a 0-chunk (that is the terminator)
// — encodeChunk of length 0 emits nothing.
TEST(StreamingBodyTests, encodeChunkOfEmptyEmitsNothing) {
    EXPECT_EQ(runI32(
        "int8[] data = M.bytes(\"\");\n"
        "int8[] c = ChunkedEncoder.encodeChunk(data, 0);\n"
        "if (c.count() != 0) return -1;\n"
        "return 1;"), 1);
}

// The last-chunk terminator is exactly "0\r\n\r\n".
TEST(StreamingBodyTests, encodeLastIsZeroTerminator) {
    EXPECT_EQ(runI32(
        "int8[] last = ChunkedEncoder.encodeLast();\n"
        "String s = heap String(#last, last.count());\n"
        "if (!s.equals(\"0\\r\\n\\r\\n\")) return -1;\n"
        "return 1;"), 1);
}

// Round-trip witness: encode three incremental pieces + the terminator,
// feed the concatenation to the chunked BodyReader, and assert it decodes
// back to the original concatenated payload — encode/decode are inverses
// over the same RFC 7230 section 4.1 wire form.
TEST(StreamingBodyTests, encodeThenChunkedDecodeRoundTrips) {
    EXPECT_EQ(runI32(
        // Build "Wiki" + "pedia" + " in chunks." as three streamed chunks.
        "int8[] p1 = M.bytes(\"Wiki\");\n"
        "int8[] p2 = M.bytes(\"pedia\");\n"
        "int8[] p3 = M.bytes(\" in chunks.\");\n"
        "int8[] c1 = ChunkedEncoder.encodeChunk(p1, 4);\n"
        "int8[] c2 = ChunkedEncoder.encodeChunk(p2, 5);\n"
        "int8[] c3 = ChunkedEncoder.encodeChunk(p3, 11);\n"
        "int8[] last = ChunkedEncoder.encodeLast();\n"
        // Feed each chunk to a fresh chunked decoder, draining as we go.
        "BodyReader br = BodyReader.forChunked();\n"
        "br.feed(c1, c1.count());\n"
        "br.feed(c2, c2.count());\n"
        "br.feed(c3, c3.count());\n"
        "br.feed(last, last.count());\n"
        "if (!br.isComplete()) return -1;\n"
        "int8[] decoded = br.drain();\n"
        "String s = heap String(#decoded, decoded.count());\n"
        "if (!s.equals(\"Wikipedia in chunks.\")) return -2;\n"
        "return 1;"), 1);
}

// ===== HttpSerializer.responseChunkedHead — head only ===================

// The streaming response head: status line + headers + the auto
// Transfer-Encoding: chunked + blank line, and NO body / NO terminator.
TEST(StreamingBodyTests, responseChunkedHeadIsHeadOnly) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.of(200);\n"
        "r.header(\"Content-Type\", \"text/plain\");\n"
        "int8[] head = HttpSerializer.responseChunkedHead(r);\n"
        "String s = heap String(#head, head.count());\n"
        "if (!s.startsWith(\"HTTP/1.1 200 OK\\r\\n\")) return -1;\n"
        "if (s.indexOf(\"Content-Type: text/plain\") < 0) return -2;\n"
        // Auto-added chunked framing.
        "if (s.indexOf(\"Transfer-Encoding: chunked\") < 0) return -3;\n"
        // Ends at the head terminator — no chunk, no 0-terminator.
        "if (!s.endsWith(\"\\r\\n\\r\\n\")) return -4;\n"
        "if (s.indexOf(\"0\\r\\n\\r\\n\\r\\n\") >= 0) return -5;\n"  // no body framing
        "return 1;"), 1);
}

// ===== RequestBodyStream — streaming upload over the reader seam =========

// A Content-Length upload streams in decoded slices off the AsyncReader
// (pre-staged, no socket) and ends with an empty terminal slice.
TEST(StreamingBodyTests, requestBodyStreamReadsContentLength) {
    EXPECT_EQ(runI32(
        "AsyncReader reader = M.staged(\"hello world\");\n"   // 11 bytes
        "BodyReader br = BodyReader.forContentLength((int64) 11);\n"
        "RequestBodyStream body = RequestBodyStream.over(br, reader);\n"
        // Drain the stream, concatenating decoded slices into `acc`.\n"
        "int8[] acc = heap int8[64];\n"
        "int32 accLen = 0;\n"
        "boolean done = false;\n"
        "while (!done) {\n"
        "    int8[] piece = body.read();\n"
        "    int32 n = piece.count();\n"
        "    if (n == 0) { done = true; }\n"
        "    int32 i = 0;\n"
        "    while (i < n) { acc[accLen] = piece[i]; accLen = accLen + 1; i = i + 1; }\n"
        "}\n"
        "if (accLen != 11) return -1;\n"
        "if (!body.isComplete()) return -2;\n"
        "String s = heap String(#acc, 11);\n"
        "if (!s.equals(\"hello world\")) return -3;\n"
        "return 1;"), 1);
}

// A chunked upload is transfer-decoded as it streams: the BodyReader sees
// the raw chunks off the reader and the stream yields the decoded body.
TEST(StreamingBodyTests, requestBodyStreamDecodesChunkedUpload) {
    EXPECT_EQ(runI32(
        // "Wikipedia" as 4+5 chunks + terminator, staged as the raw body.
        "AsyncReader reader = M.staged(\"4\\r\\nWiki\\r\\n5\\r\\npedia\\r\\n0\\r\\n\\r\\n\");\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "RequestBodyStream body = RequestBodyStream.over(br, reader);\n"
        "int8[] acc = heap int8[64];\n"
        "int32 accLen = 0;\n"
        "boolean done = false;\n"
        "while (!done) {\n"
        "    int8[] piece = body.read();\n"
        "    int32 n = piece.count();\n"
        "    if (n == 0) { done = true; }\n"
        "    int32 i = 0;\n"
        "    while (i < n) { acc[accLen] = piece[i]; accLen = accLen + 1; i = i + 1; }\n"
        "}\n"
        "if (accLen != 9) return -1;\n"            // 'Wikipedia'
        "String s = heap String(#acc, 9);\n"
        "if (!s.equals(\"Wikipedia\")) return -2;\n"
        "if (!body.isComplete()) return -3;\n"
        "return 1;"), 1);
}

// A bodyless framing (KIND_NONE) yields an immediately-empty stream.
TEST(StreamingBodyTests, requestBodyStreamNoneIsEmpty) {
    EXPECT_EQ(runI32(
        "AsyncReader reader = M.staged(\"\");\n"
        "BodyFraming f = BodyFraming.none(true);\n"
        "BodyReader br = BodyReader.forFraming(f);\n"
        "RequestBodyStream body = RequestBodyStream.over(br, reader);\n"
        "int8[] piece = body.read();\n"
        "if (piece.count() != 0) return -1;\n"
        "if (!body.isComplete()) return -2;\n"
        "return 1;"), 1);
}

// A truncated Content-Length upload (peer closes before all bytes arrive)
// raises UnexpectedEofException through the stream's endInput.
TEST(StreamingBodyTests, requestBodyStreamTruncatedThrows) {
    EXPECT_EQ(runI32(
        // Declares 11 bytes but only 5 are staged, then EOF.
        "AsyncReader reader = M.staged(\"hello\");\n"
        "BodyReader br = BodyReader.forContentLength((int64) 11);\n"
        "RequestBodyStream body = RequestBodyStream.over(br, reader);\n"
        "try {\n"
        "    boolean done = false;\n"
        "    while (!done) {\n"
        "        int8[] piece = body.read();\n"
        "        if (piece.count() == 0) { done = true; }\n"
        "    }\n"
        "    return -1;\n"                          // should have thrown
        "} catch (UnexpectedEofException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// Pipelined safety: bytes past the body boundary (the next request) are
// pushed back into the reader's staging ring, not consumed as body.
TEST(StreamingBodyTests, requestBodyStreamRestagesPipelinedBytes) {
    EXPECT_EQ(runI32(
        // 5 body bytes "hello" + the start of the next request line.
        "AsyncReader reader = M.staged(\"helloGET /next HTTP/1.1\\r\\n\");\n"
        "BodyReader br = BodyReader.forContentLength((int64) 5);\n"
        "RequestBodyStream body = RequestBodyStream.over(br, reader);\n"
        "int8[] acc = heap int8[8];\n"
        "int32 accLen = 0;\n"
        "boolean done = false;\n"
        "while (!done) {\n"
        "    int8[] piece = body.read();\n"
        "    int32 n = piece.count();\n"
        "    if (n == 0) { done = true; }\n"
        "    int32 i = 0;\n"
        "    while (i < n) { acc[accLen] = piece[i]; accLen = accLen + 1; i = i + 1; }\n"
        "}\n"
        "if (accLen != 5) return -1;\n"
        // The next request's bytes were re-staged for the connection loop.
        "int8[] next = heap int8[3];\n"
        "int32 got = reader.read(next, 0, 3);\n"
        "if (got != 3) return -2;\n"
        "if (next[0] != (int8) 71) return -3;\n"   // 'G'
        "if (next[1] != (int8) 69) return -4;\n"   // 'E'
        "if (next[2] != (int8) 84) return -5;\n"   // 'T'
        "return 1;"), 1);
}
