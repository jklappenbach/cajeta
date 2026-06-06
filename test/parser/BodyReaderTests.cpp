// NET-7.4 — cajeta.net.http.BodyReader tests (streaming body framing).
//
// The body reader is a resumable, streaming decoder over byte buffers —
// pure logic, no I/O — so it tests directly over the JIT, the same way
// HttpParserTests drives the head parser: each test compiles a small
// Cajeta `run()` that feeds wire bytes to a BodyReader and returns an
// int32 sentinel (positive on success, a distinct negative per failed
// sub-check).
//
// The chunked vectors here mirror test/net/golden/http/chunked.vectors
// byte-for-byte, so this suite pins the same RFC 7230 §4.1 corpus the
// NET-13.2 golden meta-test (GoldenVectorsTests.cpp) self-validates.
//
// Pins the NET-7.4 deliverable: "Body framing: Content-Length reader,
// chunked transfer-decoding (size lines, chunk-extensions ignored,
// trailers), and Connection: close-delimited bodies. A BodyReader that
// streams (doesn't buffer the whole body)." (plan/cajeta-net-plan.md,
// Phase 7) and its acceptance row chunkedDecodeGoldenVectors.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the body reader, the framing
// type, the parser (some tests drive parser -> reader end-to-end), and
// the body exceptions. The body must `return` int32.
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.Headers;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.net.http.HttpParser;\n"
        "import cajeta.net.http.BodyFraming;\n"
        "import cajeta.net.http.BodyReader;\n"
        "import cajeta.net.http.InvalidChunkEncodingException;\n"
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

// --- Content-Length framing --------------------------------------------

// A fixed-length body is consumed exactly; completion flips on the Nth
// byte; the decoded bytes drain byte-identically to the input.
TEST(BodyReaderTests, contentLengthConsumesExactBytes) {
    EXPECT_EQ(runI32(
        "String body = \"hello\";\n"
        "BodyReader br = BodyReader.forContentLength((int64) 5);\n"
        "boolean done = br.feed(body.bytes, body.byteLength);\n"
        "if (!done) return -1;\n"
        "if (!br.isComplete()) return -2;\n"
        "if (br.decodedLength() != 5) return -3;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 5) return -4;\n"
        "if (out[0] != (int8) 104) return -5;\n"   // 'h'
        "if (out[4] != (int8) 111) return -6;\n"   // 'o'
        "if (br.decodedLength() != 0) return -7;\n" // drain cleared it
        "return 1;"), 1);
}

// A zero-length Content-Length body is complete from construction.
TEST(BodyReaderTests, contentLengthZeroIsImmediatelyComplete) {
    EXPECT_EQ(runI32(
        "BodyReader br = BodyReader.forContentLength((int64) 0);\n"
        "if (!br.isComplete()) return -1;\n"
        "if (br.decodedLength() != 0) return -2;\n"
        "return 1;"), 1);
}

// Bytes past the declared Content-Length are NOT body — they are the
// next pipelined message, captured as leftover.
TEST(BodyReaderTests, contentLengthOvershootGoesToLeftover) {
    EXPECT_EQ(runI32(
        // 5 body bytes "hello" + the start of a next request.
        "String wire = \"helloGET /next\";\n"
        "BodyReader br = BodyReader.forContentLength((int64) 5);\n"
        "br.feed(wire.bytes, wire.byteLength);\n"
        "if (!br.isComplete()) return -1;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 5) return -2;\n"
        "if (br.leftoverLength() != 9) return -3;\n"   // "GET /next"
        "int8[] lo = br.leftover();\n"
        "if (lo[0] != (int8) 71) return -4;\n"         // 'G'
        "return 1;"), 1);
}

// A fixed body split across many feeds decodes identically to one shot,
// draining incrementally between feeds (streaming).
TEST(BodyReaderTests, contentLengthStreamsAcrossFeeds) {
    EXPECT_EQ(runI32(
        "String w = \"streaming-body-here\";\n"   // 19 bytes
        "int8[] b = w.bytes;\n"
        "int32 n = w.byteLength;\n"
        "BodyReader br = BodyReader.forContentLength((int64) n);\n"
        "int32 total = 0;\n"
        "int32 i = 0;\n"
        "int8[] one = new int8[1];\n"
        "while (i < n) {\n"
        "    one[0] = b[i];\n"
        "    br.feed(one, 1);\n"
        "    int8[] piece = br.drain();\n"          // drain each step
        "    total = total + piece.count();\n"
        "    i = i + 1;\n"
        "}\n"
        "if (!br.isComplete()) return -1;\n"
        "if (total != n) return -2;\n"              // every byte drained once
        "return 1;"), 1);
}

// A truncated Content-Length body: endInput raises UnexpectedEof.
TEST(BodyReaderTests, contentLengthTruncatedRaisesEof) {
    EXPECT_EQ(runI32(
        "String body = \"hel\";\n"                  // only 3 of 5 bytes
        "BodyReader br = BodyReader.forContentLength((int64) 5);\n"
        "br.feed(body.bytes, body.byteLength);\n"
        "if (br.isComplete()) return -1;\n"
        "try {\n"
        "    br.endInput();\n"
        "    return -2;\n"                          // should have thrown
        "} catch (UnexpectedEofException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- chunked framing: golden vectors -----------------------------------

// single: 5\r\nhello\r\n0\r\n\r\n -> "hello".
TEST(BodyReaderTests, chunkedSingleVector) {
    EXPECT_EQ(runI32(
        "String wire = \"5\\r\\nhello\\r\\n0\\r\\n\\r\\n\";\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "boolean done = br.feed(wire.bytes, wire.byteLength);\n"
        "if (!done) return -1;\n"
        "if (!br.isComplete()) return -2;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 5) return -3;\n"
        "if (out[0] != (int8) 104) return -4;\n"   // 'h'
        "if (out[4] != (int8) 111) return -5;\n"   // 'o'
        "return 1;"), 1);
}

// empty: 0\r\n\r\n -> empty body, complete.
TEST(BodyReaderTests, chunkedEmptyVector) {
    EXPECT_EQ(runI32(
        "String wire = \"0\\r\\n\\r\\n\";\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "boolean done = br.feed(wire.bytes, wire.byteLength);\n"
        "if (!done) return -1;\n"
        "if (br.decodedLength() != 0) return -2;\n"
        "return 1;"), 1);
}

// chunk-extension: 5;foo=bar\r\nhello\r\n0\r\n\r\n -> "hello"
// (the extension after ';' is parsed but ignored).
TEST(BodyReaderTests, chunkedExtensionIgnoredVector) {
    EXPECT_EQ(runI32(
        "String wire = \"5;foo=bar\\r\\nhello\\r\\n0\\r\\n\\r\\n\";\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "br.feed(wire.bytes, wire.byteLength);\n"
        "if (!br.isComplete()) return -1;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 5) return -2;\n"
        "if (out[0] != (int8) 104) return -3;\n"   // 'h'
        "return 1;"), 1);
}

// trailers: 4\r\ndata\r\n0\r\nX-Checksum: abc\r\n\r\n -> "data"
// (the trailer line is consumed but is NOT part of the body).
TEST(BodyReaderTests, chunkedTrailersConsumedNotBodyVector) {
    EXPECT_EQ(runI32(
        "String wire = \"4\\r\\ndata\\r\\n0\\r\\nX-Checksum: abc\\r\\n\\r\\n\";\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "boolean done = br.feed(wire.bytes, wire.byteLength);\n"
        "if (!done) return -1;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 4) return -2;\n"
        "if (out[0] != (int8) 100) return -3;\n"   // 'd'
        "if (out[3] != (int8) 97) return -4;\n"    // 'a'
        "return 1;"), 1);
}

// wikipedia: the multi-chunk RFC example with CRLFs *inside* the data —
// 4 Wiki / 5 pedia / e ' in\r\n\r\nchunks.' / 0 -> "Wikipedia in\r\n\r\nchunks."
TEST(BodyReaderTests, chunkedWikipediaMultiChunkVector) {
    EXPECT_EQ(runI32(
        "String wire = \"4\\r\\nWiki\\r\\n5\\r\\npedia\\r\\n"
        "e\\r\\n in\\r\\n\\r\\nchunks.\\r\\n0\\r\\n\\r\\n\";\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "br.feed(wire.bytes, wire.byteLength);\n"
        "if (!br.isComplete()) return -1;\n"
        "int8[] out = br.drain();\n"
        // "Wikipedia in\r\n\r\nchunks." = 23 bytes.
        "if (out.count() != 23) return -2;\n"
        "if (out[0] != (int8) 87) return -3;\n"    // 'W'
        "if (out[9] != (int8) 32) return -4;\n"    // ' ' (after "Wikipedia")
        "if (out[12] != (int8) 13) return -5;\n"   // CR inside the data
        "if (out[22] != (int8) 46) return -6;\n"   // '.' (final byte)
        "return 1;"), 1);
}

// --- chunked: incremental (split feed == one shot) ---------------------

// The wikipedia vector fed one byte at a time decodes identically — the
// chunked state machine resumes across every split (size line, data,
// the trailing CRLFs, the trailer blank line).
TEST(BodyReaderTests, chunkedByteAtATimeMatchesOneShot) {
    EXPECT_EQ(runI32(
        "String wire = \"4\\r\\nWiki\\r\\n5\\r\\npedia\\r\\n"
        "e\\r\\n in\\r\\n\\r\\nchunks.\\r\\n0\\r\\n\\r\\n\";\n"
        "int8[] b = wire.bytes;\n"
        "int32 n = wire.byteLength;\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "int32 i = 0;\n"
        "int8[] one = new int8[1];\n"
        "while (i < n) {\n"
        "    one[0] = b[i];\n"
        "    br.feed(one, 1);\n"
        "    i = i + 1;\n"
        "}\n"
        "if (!br.isComplete()) return -1;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 23) return -2;\n"
        "if (out[0] != (int8) 87) return -3;\n"    // 'W'
        "if (out[22] != (int8) 46) return -4;\n"   // '.'
        "return 1;"), 1);
}

// Bytes after a chunked body's terminating blank line are leftover (the
// next pipelined message), not body.
TEST(BodyReaderTests, chunkedLeftoverAfterTerminator) {
    EXPECT_EQ(runI32(
        "String wire = \"5\\r\\nhello\\r\\n0\\r\\n\\r\\nGET /next\";\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "br.feed(wire.bytes, wire.byteLength);\n"
        "if (!br.isComplete()) return -1;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 5) return -2;\n"
        "if (br.leftoverLength() != 9) return -3;\n"   // "GET /next"
        "int8[] lo = br.leftover();\n"
        "if (lo[0] != (int8) 71) return -4;\n"         // 'G'
        "return 1;"), 1);
}

// --- chunked: malformed -> InvalidChunkEncoding ------------------------

// A non-hex character in the size line is rejected.
TEST(BodyReaderTests, chunkedNonHexSizeRejected) {
    EXPECT_EQ(runI32(
        "String wire = \"XYZ\\r\\nhello\\r\\n0\\r\\n\\r\\n\";\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "try {\n"
        "    br.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (InvalidChunkEncodingException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// A chunk whose data is not followed by CRLF is rejected.
TEST(BodyReaderTests, chunkedMissingDataCrlfRejected) {
    EXPECT_EQ(runI32(
        // "5\r\nhelloXX..." — 'X' where the CR after the data must be.
        "String wire = \"5\\r\\nhelloXX0\\r\\n\\r\\n\";\n"
        "BodyReader br = BodyReader.forChunked();\n"
        "try {\n"
        "    br.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (InvalidChunkEncodingException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// A truncated chunked stream (no terminating zero chunk): endInput
// raises UnexpectedEof.
TEST(BodyReaderTests, chunkedTruncatedRaisesEof) {
    EXPECT_EQ(runI32(
        "String wire = \"5\\r\\nhello\\r\\n\";\n"   // no 0-chunk terminator
        "BodyReader br = BodyReader.forChunked();\n"
        "boolean done = br.feed(wire.bytes, wire.byteLength);\n"
        "if (done) return -1;\n"                    // not complete
        "try {\n"
        "    br.endInput();\n"
        "    return -2;\n"
        "} catch (UnexpectedEofException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- close-delimited framing -------------------------------------------

// A close-delimited body consumes every fed byte and completes only on
// endInput (the connection close IS the delimiter).
TEST(BodyReaderTests, closeDelimitedCompletesOnEndInput) {
    EXPECT_EQ(runI32(
        "String a = \"chunk-one \";\n"
        "String b = \"chunk-two\";\n"
        "BodyReader br = BodyReader.forClose();\n"
        "boolean d1 = br.feed(a.bytes, a.byteLength);\n"
        "if (d1) return -1;\n"                       // never completes from feed
        "boolean d2 = br.feed(b.bytes, b.byteLength);\n"
        "if (d2) return -2;\n"
        "br.endInput();\n"
        "if (!br.isComplete()) return -3;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 19) return -4;\n"        // 10 + 9 bytes
        "if (out[0] != (int8) 99) return -5;\n"      // 'c'
        "return 1;"), 1);
}

// --- end-to-end: parser head -> body reader ----------------------------

// The intended hand-off: HttpParser parses the head + framing, the
// leftover seeds a BodyReader built from that framing, and the body
// decodes. A chunked response end-to-end.
TEST(BodyReaderTests, parserFramingDrivesReaderEndToEnd) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked\\r\\n"
        "\\r\\n5\\r\\nhello\\r\\n0\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "if (!p.isComplete()) return -1;\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isChunked()) return -2;\n"
        "BodyReader br = BodyReader.forFraming(f);\n"
        "br.feed(p.leftover(), p.leftoverLength());\n"
        "if (!br.isComplete()) return -3;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 5) return -4;\n"
        "if (out[0] != (int8) 104) return -5;\n"   // 'h'
        "return 1;"), 1);
}

// forFraming on a Content-Length framing consumes the leftover body.
TEST(BodyReaderTests, parserContentLengthFramingDrivesReader) {
    EXPECT_EQ(runI32(
        "String wire = \"HTTP/1.1 200 OK\\r\\nContent-Length: 5\\r\\n\\r\\nhello\";\n"
        "HttpParser p = HttpParser.forResponse();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isContentLength()) return -1;\n"
        "BodyReader br = BodyReader.forFraming(f);\n"
        "boolean done = br.feed(p.leftover(), p.leftoverLength());\n"
        "if (!done) return -2;\n"
        "int8[] out = br.drain();\n"
        "if (out.count() != 5) return -3;\n"
        "return 1;"), 1);
}

// forFraming on a KIND_NONE framing is immediately complete with no body.
TEST(BodyReaderTests, parserNoneFramingIsComplete) {
    EXPECT_EQ(runI32(
        "String wire = \"GET /hello.txt HTTP/1.1\\r\\nHost: h.test\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "BodyFraming f = p.getFraming();\n"
        "if (!f.isNone()) return -1;\n"
        "BodyReader br = BodyReader.forFraming(f);\n"
        "if (!br.isComplete()) return -2;\n"
        "if (br.decodedLength() != 0) return -3;\n"
        "return 1;"), 1);
}
