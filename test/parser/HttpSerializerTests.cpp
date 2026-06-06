// NET-7.5 — cajeta.net.http.HttpSerializer tests.
//
// The serializer is a pure codec over byte buffers (no I/O — the
// AsyncWriter target is a later Phase-3 shim), so it tests directly
// over the JIT exactly like HttpMessageTests / HttpHeadersTests: each
// test compiles a small Cajeta `run()` that builds a message, serializes
// it, and byte-compares the result against an inline golden wire form,
// returning an int32 sentinel — `1` on an exact match, a distinct
// negative per failed sub-check (or the negated 1-based mismatch index
// from the byte-equality helper).
//
// Pins the NET-7.5 deliverable: "Serializer: write a request/response
// head + Content-Length or chunked transfer-encoding body ... correct
// Host, Connection, Date defaults." (plan/cajeta-net-plan.md, Phase 7).
//
// The request/response golden wire forms here are byte-identical to the
// corpus under test/net/golden/http/ (request-get-simple.http,
// request-post-contentlength.http, response-200-contentlength.http) — a
// round-trip witness that the serializer emits the same bytes the
// parser corpus consumes.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the HTTP message types, the
// serializer, the header map, the URI type, and String. The body must
// `return` int32.
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.Headers;\n"
        "import cajeta.net.uri.Uri;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.net.http.HttpSerializer;\n"
        "public final class M {\n"
        "    // Compare produced[0..n) against expect's bytes; return 1 on\n"
        "    // an exact match (same length + same bytes), else the negated\n"
        "    // 1-based index of the first divergence (length mismatch maps\n"
        "    // to -(min+1)).\n"
        "    static int32 eq(int8[] produced, int32 n, String expect) {\n"
        "        int32 e = expect.byteLength;\n"
        "        int32 m = n < e ? n : e;\n"
        "        int32 i = 0;\n"
        "        while (i < m) {\n"
        "            if (produced[i] != expect.byteAt(i)) { return -(i + 1); }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (n != e) { return -(m + 1); }\n"
        "        return 1;\n"
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

// --- request serialization ---------------------------------------------

// A simple GET with three headers serializes to the canonical
// request-line + header block + blank line, no body. Byte-identical to
// test/net/golden/http/request-get-simple.http.
TEST(HttpSerializerTests, getSimpleGoldenVector) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.get(\"/hello.txt\");\n"
        "r.header(\"Host\", \"www.example.test\");\n"
        "r.header(\"User-Agent\", \"cajeta-net/1.0\");\n"
        "r.header(\"Accept\", \"*/*\");\n"
        "int8[] wire = HttpSerializer.request(r);\n"
        // header names are lowercased by the map (wire-legal, RFC 7230 §3.2).
        "String expect = \"GET /hello.txt HTTP/1.1\\r\\n\"\n"
        "    + \"host: www.example.test\\r\\n\"\n"
        "    + \"user-agent: cajeta-net/1.0\\r\\n\"\n"
        "    + \"accept: */*\\r\\n\"\n"
        "    + \"\\r\\n\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// A POST with an in-memory body gets an auto Content-Length (the caller
// didn't set one) and the body bytes verbatim. The auto length lands
// after the caller's headers, in insertion order.
TEST(HttpSerializerTests, postContentLengthAutoFramed) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.post(\"/submit\");\n"
        "r.header(\"Host\", \"api.example.test\");\n"
        "r.header(\"Content-Type\", \"application/json\");\n"
        "int8[] payload = new int8[17];\n"
        // {"name":"cajeta"}
        "String json = \"{\\\"name\\\":\\\"cajeta\\\"}\";\n"
        "int32 k = 0;\n"
        "while (k < 17) { payload[k] = json.byteAt(k); k = k + 1; }\n"
        "r.body(payload, 17);\n"
        "int8[] wire = HttpSerializer.request(r);\n"
        "String expect = \"POST /submit HTTP/1.1\\r\\n\"\n"
        "    + \"host: api.example.test\\r\\n\"\n"
        "    + \"content-type: application/json\\r\\n\"\n"
        "    + \"content-length: 17\\r\\n\"\n"
        "    + \"\\r\\n\"\n"
        "    + \"{\\\"name\\\":\\\"cajeta\\\"}\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// An explicit Content-Length the caller already set is NOT duplicated —
// the serializer only supplies the default when absent.
TEST(HttpSerializerTests, explicitContentLengthNotDuplicated) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.post(\"/x\");\n"
        "r.setHeader(\"Content-Length\", \"3\");\n"
        "int8[] payload = new int8[3];\n"
        "payload[0] = (int8) 97; payload[1] = (int8) 98; payload[2] = (int8) 99;\n"
        "r.body(payload, 3);\n"
        "int8[] wire = HttpSerializer.request(r);\n"
        "String expect = \"POST /x HTTP/1.1\\r\\n\"\n"
        "    + \"content-length: 3\\r\\n\"\n"
        "    + \"\\r\\n\"\n"
        "    + \"abc\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// A bodyless GET emits no Content-Length and no body — just the head.
TEST(HttpSerializerTests, bodylessRequestHasNoLengthHeader) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.get(\"/\");\n"
        "int8[] wire = HttpSerializer.request(r);\n"
        "String expect = \"GET / HTTP/1.1\\r\\n\\r\\n\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// A present-but-empty body (hasBody, length 0) frames Content-Length: 0
// and zero body bytes — distinct from an absent body.
TEST(HttpSerializerTests, presentEmptyBodyFramesZeroLength) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.post(\"/empty\");\n"
        "int8[] payload = new int8[0];\n"
        "r.body(payload, 0);\n"
        "int8[] wire = HttpSerializer.request(r);\n"
        "String expect = \"POST /empty HTTP/1.1\\r\\n\"\n"
        "    + \"content-length: 0\\r\\n\"\n"
        "    + \"\\r\\n\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// --- response serialization --------------------------------------------

// A 200 OK with a text body + headers serializes byte-identical to
// test/net/golden/http/response-200-contentlength.http (Content-Length
// is the caller's explicit header there, so no auto length is added).
TEST(HttpSerializerTests, response200GoldenVector) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.ok();\n"
        "r.header(\"Content-Type\", \"text/plain\");\n"
        "r.header(\"Content-Length\", \"13\");\n"
        "r.header(\"Connection\", \"keep-alive\");\n"
        "int8[] payload = new int8[13];\n"
        "String b = \"Hello, world!\";\n"
        "int32 k = 0;\n"
        "while (k < 13) { payload[k] = b.byteAt(k); k = k + 1; }\n"
        "r.body(payload, 13);\n"
        "int8[] wire = HttpSerializer.response(r);\n"
        "String expect = \"HTTP/1.1 200 OK\\r\\n\"\n"
        "    + \"content-type: text/plain\\r\\n\"\n"
        "    + \"content-length: 13\\r\\n\"\n"
        "    + \"connection: keep-alive\\r\\n\"\n"
        "    + \"\\r\\n\"\n"
        "    + \"Hello, world!\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// A bodyless 204 emits the status line + blank line, no Content-Length.
TEST(HttpSerializerTests, response204NoBodyNoLength) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.of(204);\n"
        "int8[] wire = HttpSerializer.response(r);\n"
        "String expect = \"HTTP/1.1 204 No Content\\r\\n\\r\\n\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// An unknown status code emits an empty reason phrase — the SP before it
// is still present per the grammar (`HTTP/1.1 599 \r\n`).
TEST(HttpSerializerTests, unknownStatusEmptyReasonKeepsSpace) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.of(599);\n"
        "int8[] wire = HttpSerializer.response(r);\n"
        "String expect = \"HTTP/1.1 599 \\r\\n\\r\\n\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// --- chunked transfer-encoding -----------------------------------------

// Chunked framing of a non-empty body: auto Transfer-Encoding header,
// one size-hex chunk, then the 0\r\n\r\n last-chunk terminator. The size
// line is lowercase hex with no leading zeros (255 → "ff" would; here 5
// → "5"). Golden hex framing per RFC 7230 §4.1.
TEST(HttpSerializerTests, chunkedSingleChunkGoldenVector) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.ok();\n"
        "int8[] payload = new int8[5];\n"
        "String b = \"hello\";\n"
        "int32 k = 0;\n"
        "while (k < 5) { payload[k] = b.byteAt(k); k = k + 1; }\n"
        "r.body(payload, 5);\n"
        "int8[] wire = HttpSerializer.responseChunked(r);\n"
        "String expect = \"HTTP/1.1 200 OK\\r\\n\"\n"
        "    + \"transfer-encoding: chunked\\r\\n\"\n"
        "    + \"\\r\\n\"\n"
        "    + \"5\\r\\n\"\n"
        "    + \"hello\\r\\n\"\n"
        "    + \"0\\r\\n\\r\\n\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// A 16-byte body exercises the multi-hex-digit chunk size (16 → "10").
TEST(HttpSerializerTests, chunkedHexSizeIsLowercaseNoLeadingZero) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.ok();\n"
        "int8[] payload = new int8[16];\n"
        "int32 k = 0;\n"
        "while (k < 16) { payload[k] = (int8) 88; k = k + 1; }\n"   // 'X' * 16
        "r.body(payload, 16);\n"
        "int8[] wire = HttpSerializer.responseChunked(r);\n"
        "String expect = \"HTTP/1.1 200 OK\\r\\n\"\n"
        "    + \"transfer-encoding: chunked\\r\\n\"\n"
        "    + \"\\r\\n\"\n"
        "    + \"10\\r\\n\"\n"
        "    + \"XXXXXXXXXXXXXXXX\\r\\n\"\n"
        "    + \"0\\r\\n\\r\\n\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// An empty chunked body emits just the head + the bare last-chunk
// terminator (no zero-size data chunk, which would terminate early).
TEST(HttpSerializerTests, chunkedEmptyBodyIsTerminatorOnly) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.post(\"/stream\");\n"
        "int8[] payload = new int8[0];\n"
        "r.body(payload, 0);\n"
        "int8[] wire = HttpSerializer.requestChunked(r);\n"
        "String expect = \"POST /stream HTTP/1.1\\r\\n\"\n"
        "    + \"transfer-encoding: chunked\\r\\n\"\n"
        "    + \"\\r\\n\"\n"
        "    + \"0\\r\\n\\r\\n\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// An explicit Transfer-Encoding the caller set is not duplicated.
TEST(HttpSerializerTests, explicitTransferEncodingNotDuplicated) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.ok();\n"
        "r.setHeader(\"Transfer-Encoding\", \"chunked\");\n"
        "int8[] payload = new int8[1];\n"
        "payload[0] = (int8) 90;\n"   // 'Z'
        "r.body(payload, 1);\n"
        "int8[] wire = HttpSerializer.responseChunked(r);\n"
        "String expect = \"HTTP/1.1 200 OK\\r\\n\"\n"
        "    + \"transfer-encoding: chunked\\r\\n\"\n"
        "    + \"\\r\\n\"\n"
        "    + \"1\\r\\n\"\n"
        "    + \"Z\\r\\n\"\n"
        "    + \"0\\r\\n\\r\\n\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// In the length-framed path an explicit Transfer-Encoding suppresses the
// auto Content-Length (RFC 7230 §3.3.2: a sender MUST NOT send both).
TEST(HttpSerializerTests, transferEncodingSuppressesAutoLength) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.ok();\n"
        "r.setHeader(\"Transfer-Encoding\", \"chunked\");\n"
        "int8[] payload = new int8[3];\n"
        "payload[0] = (int8) 97; payload[1] = (int8) 98; payload[2] = (int8) 99;\n"
        "r.body(payload, 3);\n"
        // length-framed writeResponse: must NOT inject content-length.
        "int8[] wire = HttpSerializer.response(r);\n"
        "String expect = \"HTTP/1.1 200 OK\\r\\n\"\n"
        "    + \"transfer-encoding: chunked\\r\\n\"\n"
        "    + \"\\r\\n\"\n"
        "    + \"abc\";\n"
        "return M.eq(wire, wire.count(), expect);"), 1);
}

// toStringValue() hands back the same bytes as toBytes() as a String —
// the text-protocol debug view round-trips.
TEST(HttpSerializerTests, toStringValueMatchesBytes) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.get(\"/\");\n"
        "HttpSerializer s = heap HttpSerializer();\n"
        "s.writeRequest(r);\n"
        "String text = s.toStringValue();\n"
        "if (!text.equals(\"GET / HTTP/1.1\\r\\n\\r\\n\")) return -1;\n"
        "return 1;"), 1);
}
