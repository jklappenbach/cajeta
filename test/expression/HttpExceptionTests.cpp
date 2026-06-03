// Tests for the cajeta.net.http HTTP-exception hierarchy — plan item
// NET-7.7 (`HttpException` intermediate + `MalformedMessage` /
// `HeadersTooLarge` / `InvalidChunkEncoding` / `UnexpectedEof` reparented
// under it, off `cajeta.net.NetException`).
//
// NET-7.3 / NET-7.4 originally rooted the four leaf exceptions directly on
// `cajeta.error.RecoverableException` as placeholders, because the
// NET-7.7 base (the intermediate `HttpException` over the NET-1.8
// `NetException` root) had not yet been built. NET-7.7 introduces
// `HttpException extends NetException` and reparents the four leaves onto
// it. These tests pin the reparent contract, mirroring
// UriMalformedExceptionTests (NET-6.5):
//
//   1. each thrown leaf is catchable as the intermediate `HttpException`
//      (proves the new middle node), and
//   2. each is catchable as the `NetException` *root* (proves the chain
//      reaches the networking root, not RecoverableException directly),
//      and
//   3. each carries the inherited `kind == NetException.KIND_INVALID` (12)
//      — an HTTP parse failure is the "invalid input" bucket, not an
//      errno-derived socket error, and
//   4. the NET-7.3/7.4 detail-field contracts (position / limit+observed
//      / bytesBuffered / chunkIndex) are unchanged: the carried detail
//      still rides on the instance even when caught at a base type, and
//   5. each is still catchable by its specific subtype (no regression to
//      the existing HttpParserTests catch forms).
//
// Harness mirrors HttpParserTests / UriMalformedExceptionTests: compile a
// small cajeta source through the JIT, call run() -> int32. Where a real
// thrower exists (the NET-7.3 parser raises HeadersTooLarge /
// MalformedMessage / UnexpectedEof on the wire), we trigger through it;
// InvalidChunkEncoding's thrower is NET-7.4 (the chunked body reader, not
// yet built), so its reparent is pinned by direct construction + throw —
// the contract under test here is the *class hierarchy + kind*, which is
// exactly what NET-7.7 owns.
//
// The kind is asserted as the literal ordinal 12 (mirroring
// `enum cajeta_net_err`), not `NetException.KIND_INVALID`, deliberately —
// NetExceptionTests / UriMalformedExceptionTests keep the same
// independence from cross-class static-constant resolution in this JIT
// harness. The named constant is the source-of-truth in the .cajeta files.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.NetException;\n"
        "import cajeta.net.http.HttpException;\n"
        "import cajeta.net.http.HttpParser;\n"
        "import cajeta.net.http.HttpParserLimits;\n"
        "import cajeta.net.http.MalformedMessageException;\n"
        "import cajeta.net.http.HeadersTooLargeException;\n"
        "import cajeta.net.http.InvalidChunkEncodingException;\n"
        "import cajeta.net.http.UnexpectedEofException;\n"
        "public final class H {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.H");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// =======================================================================
// MalformedMessageException  (real thrower: HttpParser on a bad head)
// =======================================================================

// A header line with no ':' separator is a malformed head — the parser
// raises MalformedMessageException. Catching it as the intermediate
// HttpException proves the new middle node of the NET-7.7 chain.
TEST(HttpExceptionTests, malformedMessageCaughtAsHttpException) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nbadheaderline\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return 0;\n"
        "} catch (HttpException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// ...and as the NetException root (the chain reaches the networking root).
TEST(HttpExceptionTests, malformedMessageCaughtAsNetExceptionRoot) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nbadheaderline\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return 0;\n"
        "} catch (NetException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// ...carries kind == KIND_INVALID (12), read through the NetException base.
TEST(HttpExceptionTests, malformedMessageCarriesKindInvalidViaBase) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nbadheaderline\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"
        "}"), 12);
}

// ...and is still catchable by the specific subtype, with its NET-7.3
// `position` detail intact (>= 0, tied to the offending byte).
TEST(HttpExceptionTests, malformedMessageStillCaughtBySubtypeWithPosition) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nbadheaderline\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (MalformedMessageException e) {\n"
        "    if (e.position < (int64) 0) return -2;\n"
        "    return 1;\n"
        "}"), 1);
}

// =======================================================================
// HeadersTooLargeException  (real thrower: HttpParser over a tight limit)
// =======================================================================

// A head over the configured maxHeadBytes ceiling raises
// HeadersTooLargeException — catchable as the intermediate HttpException.
TEST(HttpExceptionTests, headersTooLargeCaughtAsHttpException) {
    EXPECT_EQ(runI32(
        "HttpParserLimits lim = heap HttpParserLimits();\n"
        "lim.maxHeadBytes = 8;\n"
        "String wire = \"GET /aaaaaaaaaaaaaaaaaaaa HTTP/1.1\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequestWithLimits(lim);\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return 0;\n"
        "} catch (HttpException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// ...as the NetException root.
TEST(HttpExceptionTests, headersTooLargeCaughtAsNetExceptionRoot) {
    EXPECT_EQ(runI32(
        "HttpParserLimits lim = heap HttpParserLimits();\n"
        "lim.maxHeadBytes = 8;\n"
        "String wire = \"GET /aaaaaaaaaaaaaaaaaaaa HTTP/1.1\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequestWithLimits(lim);\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return 0;\n"
        "} catch (NetException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// ...kind == KIND_INVALID (12) via the base.
TEST(HttpExceptionTests, headersTooLargeCarriesKindInvalidViaBase) {
    EXPECT_EQ(runI32(
        "HttpParserLimits lim = heap HttpParserLimits();\n"
        "lim.maxHeadBytes = 8;\n"
        "String wire = \"GET /aaaaaaaaaaaaaaaaaaaa HTTP/1.1\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequestWithLimits(lim);\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"
        "}"), 12);
}

// ...subtype catch keeps its (limit, observed) detail across the reparent:
// the crossed ceiling is the 8 we configured.
TEST(HttpExceptionTests, headersTooLargeStillCaughtBySubtypeWithLimit) {
    EXPECT_EQ(runI32(
        "HttpParserLimits lim = heap HttpParserLimits();\n"
        "lim.maxHeadBytes = 8;\n"
        "String wire = \"GET /aaaaaaaaaaaaaaaaaaaa HTTP/1.1\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequestWithLimits(lim);\n"
        "try {\n"
        "    p.feed(wire.bytes, wire.byteLength);\n"
        "    return -1;\n"
        "} catch (HeadersTooLargeException e) {\n"
        "    return (int32) e.limit;\n"
        "}"), 8);
}

// =======================================================================
// UnexpectedEofException  (real thrower: HttpParser.endInput, no head term)
// =======================================================================

// endInput() on a head that never terminated raises UnexpectedEof —
// catchable as the intermediate HttpException.
TEST(HttpExceptionTests, unexpectedEofCaughtAsHttpException) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nHost: h\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "try {\n"
        "    p.endInput();\n"
        "    return 0;\n"
        "} catch (HttpException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// ...as the NetException root.
TEST(HttpExceptionTests, unexpectedEofCaughtAsNetExceptionRoot) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nHost: h\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "try {\n"
        "    p.endInput();\n"
        "    return 0;\n"
        "} catch (NetException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// ...kind == KIND_INVALID (12) via the base.
TEST(HttpExceptionTests, unexpectedEofCarriesKindInvalidViaBase) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nHost: h\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "try {\n"
        "    p.endInput();\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"
        "}"), 12);
}

// ...subtype catch keeps its bytesBuffered detail (> 0 head bytes seen).
TEST(HttpExceptionTests, unexpectedEofStillCaughtBySubtypeWithBytesBuffered) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nHost: h\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "p.feed(wire.bytes, wire.byteLength);\n"
        "try {\n"
        "    p.endInput();\n"
        "    return -1;\n"
        "} catch (UnexpectedEofException e) {\n"
        "    if (e.bytesBuffered <= (int64) 0) return -2;\n"
        "    return 1;\n"
        "}"), 1);
}

// =======================================================================
// InvalidChunkEncodingException  (NET-7.4 thrower not yet built — the
// NET-7.7 contract under test is the class hierarchy + kind, pinned by
// direct construction + throw)
// =======================================================================

// Catchable as the intermediate HttpException.
TEST(HttpExceptionTests, invalidChunkCaughtAsHttpException) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap InvalidChunkEncodingException(\"bad chunk size\", (int64) 7);\n"
        "} catch (HttpException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// Catchable as the NetException root.
TEST(HttpExceptionTests, invalidChunkCaughtAsNetExceptionRoot) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap InvalidChunkEncodingException(\"bad chunk size\", (int64) 7);\n"
        "} catch (NetException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// Carries kind == KIND_INVALID (12) via the base.
TEST(HttpExceptionTests, invalidChunkCarriesKindInvalidViaBase) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap InvalidChunkEncodingException(\"bad chunk size\", (int64) 7);\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"
        "}"), 12);
}

// Subtype catch preserves its NET-7.4 `position` detail (the 7 we built).
TEST(HttpExceptionTests, invalidChunkStillCaughtBySubtypeWithPosition) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap InvalidChunkEncodingException(\"bad chunk size\", (int64) 7);\n"
        "} catch (InvalidChunkEncodingException e) {\n"
        "    return (int32) e.position;\n"
        "}"), 7);
}

// =======================================================================
// Intermediate HttpException itself
// =======================================================================

// A bare HttpException (an HTTP fault with no more-specific leaf) is
// catchable as the NetException root and stamps kind == KIND_INVALID (12).
TEST(HttpExceptionTests, bareHttpExceptionCaughtAsNetExceptionWithKind) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap HttpException(\"generic http fault\");\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"
        "}"), 12);
}

// A leaf is NOT swallowed by an unrelated sibling catch: a
// MalformedMessageException must not be caught as InvalidChunkEncoding —
// it falls through to the HttpException base. (Guards against a flattened
// hierarchy where every leaf accidentally unifies.)
TEST(HttpExceptionTests, leafNotCaughtByUnrelatedSibling) {
    EXPECT_EQ(runI32(
        "String wire = \"GET / HTTP/1.1\\r\\nbadheaderline\\r\\n\\r\\n\";\n"
        "HttpParser p = HttpParser.forRequest();\n"
        "try {\n"
        "    try {\n"
        "        p.feed(wire.bytes, wire.byteLength);\n"
        "        return -1;\n"
        "    } catch (InvalidChunkEncodingException e) {\n"
        "        return -2;\n"   // must NOT be caught here
        "    }\n"
        "} catch (HttpException e) {\n"
        "    return 1;\n"        // falls through to the shared base
        "}"), 1);
}
