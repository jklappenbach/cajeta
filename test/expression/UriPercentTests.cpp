// Tests for cajeta.io.net.uri percent-encoding — plan item NET-6.2.
//
// Scope: PercentCodec.encode / PercentCodec.decode and the
// Uri.percentEncode / Uri.percentDecode facades, exercising the
// component-specific safe sets (UriComponent), UTF-8 byte-wise
// escaping, uppercase-hex output, strict-decode rejection of
// malformed escapes, and the encode->decode round-trip identity.
// NET-6.1 (Uri.parse) is covered by UriParseTests.cpp and is not
// re-exercised here.
//
// Harness mirrors UriParseTests / PathTests: compile a small cajeta
// source through the JIT, call run() -> int32. String checks use
// String.equals and return 1 on match / 0 otherwise; malformed-input
// tests catch MalformedUriException and return 1.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.P");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Wrap a method body in a class importing the percent-encoding surface.
// The body must `return` an int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.uri.Uri;\n"
           "import cajeta.io.net.uri.PercentCodec;\n"
           "import cajeta.io.net.uri.UriComponent;\n"
           "import cajeta.io.net.uri.MalformedUriException;\n"
           "public final class P {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- Unreserved set is never escaped -----------------------------------

TEST(UriPercentTests, unreservedPassThrough) {
    // ALPHA / DIGIT / - . _ ~ are raw in every component.
    EXPECT_EQ(runI32(makeSource(
        "String s #= Uri.percentEncode(\"AZaz09-._~\", UriComponent.QUERY);\n"
        "return s.equals(\"AZaz09-._~\") ? 1 : 0;")), 1);
}

// --- Space is escaped to %20 (uppercase hex) ---------------------------

TEST(UriPercentTests, spaceEncodesToPercent20) {
    EXPECT_EQ(runI32(makeSource(
        "String s #= Uri.percentEncode(\"a b\", UriComponent.PATH);\n"
        "return s.equals(\"a%20b\") ? 1 : 0;")), 1);
}

TEST(UriPercentTests, hexOutputIsUppercase) {
    // '?' = 0x3F. In USERINFO it is not safe → %3F (uppercase F).
    EXPECT_EQ(runI32(makeSource(
        "String s #= Uri.percentEncode(\"?\", UriComponent.USERINFO);\n"
        "return s.equals(\"%3F\") ? 1 : 0;")), 1);
}

// --- Component-specific safe sets DIFFER --------------------------------
// → UriTests.componentEncodingSetsDiffer (plan acceptance)

TEST(UriPercentTests, slashRawInPathEncodedInSegment) {
    // '/' is the segment separator: raw in PATH, escaped in SEGMENT.
    EXPECT_EQ(runI32(makeSource(
        "String p #= Uri.percentEncode(\"a/b\", UriComponent.PATH);\n"
        "String s #= Uri.percentEncode(\"a/b\", UriComponent.SEGMENT);\n"
        "if (!p.equals(\"a/b\")) { return 0; }\n"
        "return s.equals(\"a%2Fb\") ? 1 : 0;")), 1);
}

TEST(UriPercentTests, ampEqRawInQueryEncodedInQueryParam) {
    // '&' and '=' are query separators: raw in QUERY, escaped in
    // QUERY_PARAM so a value can't be mis-split by the NET-6.3 parser.
    EXPECT_EQ(runI32(makeSource(
        "String q #= Uri.percentEncode(\"a&b=c\", UriComponent.QUERY);\n"
        "String v #= Uri.percentEncode(\"a&b=c\", UriComponent.QUERY_PARAM);\n"
        "if (!q.equals(\"a&b=c\")) { return 0; }\n"
        "return v.equals(\"a%26b%3Dc\") ? 1 : 0;")), 1);
}

TEST(UriPercentTests, colonRawInUserinfoEncodedInHost) {
    // ':' is allowed raw in userinfo but not in reg-name host.
    EXPECT_EQ(runI32(makeSource(
        "String u #= Uri.percentEncode(\"a:b\", UriComponent.USERINFO);\n"
        "String h #= Uri.percentEncode(\"a:b\", UriComponent.HOST);\n"
        "if (!u.equals(\"a:b\")) { return 0; }\n"
        "return h.equals(\"a%3Ab\") ? 1 : 0;")), 1);
}

// --- UTF-8 awareness: multi-byte code points escape byte-by-byte -------
// Build the multi-byte input from explicit bytes (UTF-8 of U+00E9 'é'
// = 0xC3 0xA9) so the test does not depend on \u string-literal
// lowering — the byte view is exactly what percent-encoding operates on.

TEST(UriPercentTests, utf8MultibyteEncodesEachByte) {
    EXPECT_EQ(runI32(makeSource(
        "int8[] b = heap int8[2];\n"
        "b[0] = (int8) 0xC3;\n"
        "b[1] = (int8) 0xA9;\n"
        "String raw = heap String(#b, 2);\n"
        "String s #= Uri.percentEncode(raw, UriComponent.QUERY);\n"
        "return s.equals(\"%C3%A9\") ? 1 : 0;")), 1);
}

TEST(UriPercentTests, utf8DecodeReconstructsBytes) {
    // "%C3%A9" decodes back to the 2-byte sequence 0xC3 0xA9.
    EXPECT_EQ(runI32(makeSource(
        "String s #= Uri.percentDecode(\"%C3%A9\");\n"
        "if (s.byteLength() != 2) { return 0; }\n"
        "int32 b0 = ((int32) s.byteAt(0)) & 0xff;\n"
        "int32 b1 = ((int32) s.byteAt(1)) & 0xff;\n"
        "return (b0 == 0xC3 && b1 == 0xA9) ? 1 : 0;")), 1);
}

// --- Decode basics ------------------------------------------------------

TEST(UriPercentTests, decodePercent20ToSpace) {
    EXPECT_EQ(runI32(makeSource(
        "String s #= Uri.percentDecode(\"a%20b\");\n"
        "return s.equals(\"a b\") ? 1 : 0;")), 1);
}

TEST(UriPercentTests, decodeAcceptsLowercaseHex) {
    // '%3f' (lowercase) decodes to '?' just like '%3F'.
    EXPECT_EQ(runI32(makeSource(
        "String s #= Uri.percentDecode(\"%3f\");\n"
        "return s.equals(\"?\") ? 1 : 0;")), 1);
}

TEST(UriPercentTests, decodePlusStaysLiteral) {
    // RFC 3986: '+' is NOT space; the generic decoder leaves it alone.
    EXPECT_EQ(runI32(makeSource(
        "String s #= Uri.percentDecode(\"a+b\");\n"
        "return s.equals(\"a+b\") ? 1 : 0;")), 1);
}

// --- Round-trip identity ------------------------------------------------
// → UriTests.percentEncodeDecodeIsIdentity (plan acceptance)

TEST(UriPercentTests, encodeDecodeRoundTripReservedAndUtf8) {
    // A corpus mixing ASCII delimiters and raw UTF-8 bytes built
    // explicitly (0xC3 0xA9 = 'é'), avoiding \u string-literal reliance.
    EXPECT_EQ(runI32(makeSource(
        "int8[] b = heap int8[12];\n"
        "b[0]=(int8)97; b[1]=(int8)32; b[2]=(int8)47; b[3]=(int8)63;\n"   // 'a',' ','/','?'
        "b[4]=(int8)35; b[5]=(int8)38; b[6]=(int8)61; b[7]=(int8)43;\n"   // '#','&','=','+'
        "b[8]=(int8)0xC3; b[9]=(int8)0xA9;\n"                             // UTF-8 'é'
        "b[10]=(int8)91; b[11]=(int8)93;\n"                              // '[',']'
        "String raw = heap String(#b, 12);\n"
        "String enc #= Uri.percentEncode(raw, UriComponent.QUERY_PARAM);\n"
        "String dec #= Uri.percentDecode(enc);\n"
        "return dec.equals(raw) ? 1 : 0;")), 1);
}

TEST(UriPercentTests, encodeDecodeRoundTripPathComponent) {
    EXPECT_EQ(runI32(makeSource(
        "String raw = \"seg ment/with space/x\";\n"
        "String enc #= Uri.percentEncode(raw, UriComponent.PATH);\n"
        "String dec #= Uri.percentDecode(enc);\n"
        "return dec.equals(raw) ? 1 : 0;")), 1);
}

// --- Strict decode rejects malformed escapes ----------------------------

TEST(UriPercentTests, decodeTruncatedEscapeRejected) {
    // A '%' with only one following byte.
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    String s #= Uri.percentDecode(\"a%2\");\n"
        "    return 0;\n"
        "} catch (MalformedUriException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(UriPercentTests, decodeNonHexEscapeRejected) {
    // '%2g' — 'g' is not a hex digit.
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    String s #= Uri.percentDecode(\"%2g\");\n"
        "    return 0;\n"
        "} catch (MalformedUriException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(UriPercentTests, decodeTruncatedEscapeCitesPosition) {
    // "ab%4" — the '%' is at byte index 2.
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    String s #= Uri.percentDecode(\"ab%4\");\n"
        "    return -1;\n"
        "} catch (MalformedUriException e) {\n"
        "    return (int32) e.position;\n"
        "}")), 2);
}
