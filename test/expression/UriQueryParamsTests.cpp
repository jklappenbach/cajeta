// Tests for cajeta.io.net.uri query-param multi-map — plan item NET-6.3.
//
// Scope: QueryParams.parse / parseStrict (parse "?k=v&k2=v2" into an
// ordered, duplicate-key preserving multi-map) and QueryParams.toString
// (recompose a query string with correct QUERY_PARAM percent-encoding),
// plus the add/set/remove/getAll/getFirst accessors. Exercises order +
// duplicate-key preservation (plan acceptance
// UriTests.queryMultiMapPreservesOrderAndDuplicates), bare-flag (no '=')
// segments, empty-segment skipping, the form-urlencoded '+'<->space
// convention vs strict RFC '+'-literal mode, percent-decoded keys/values,
// QUERY_PARAM re-encoding of embedded separators, and the
// parse->toString round-trip.
//
// Harness mirrors UriPercentTests / UriParseTests: compile a small cajeta
// source through the JIT, call run() -> int32, returning 1 on the
// asserted property and 0 otherwise.

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

// Wrap a method body in a class importing the query-param surface.
// The body must `return` an int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.uri.Uri;\n"
           "import cajeta.io.net.uri.QueryParams;\n"
           "import cajeta.io.net.uri.MalformedUriException;\n"
           "public final class P {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- Basic parse: key/value pairs in order ------------------------------

TEST(UriQueryParamsTests, parseTwoPairs) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"x=1&y=2\");\n"
        "if (q.size() != 2) { return 0; }\n"
        "if (!q.keyAt(0).equals(\"x\")) { return 0; }\n"
        "if (!q.valueAt(0).equals(\"1\")) { return 0; }\n"
        "if (!q.keyAt(1).equals(\"y\")) { return 0; }\n"
        "return q.valueAt(1).equals(\"2\") ? 1 : 0;")), 1);
}

// --- Order + duplicate keys preserved -----------------------------------
// → UriTests.queryMultiMapPreservesOrderAndDuplicates (plan acceptance)

TEST(UriQueryParamsTests, duplicateKeysPreserveOrder) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"tag=a&tag=b&tag=c\");\n"
        "if (q.size() != 3) { return 0; }\n"
        "if (q.countFor(\"tag\") != 3) { return 0; }\n"
        "String[] all = q.getAll(\"tag\");\n"
        "if (all.count() != 3) { return 0; }\n"
        "if (!all[0].equals(\"a\")) { return 0; }\n"
        "if (!all[1].equals(\"b\")) { return 0; }\n"
        "return all[2].equals(\"c\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, getFirstReturnsFirstOccurrence) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"k=first&k=second\");\n"
        "return q.getFirst(\"k\").equals(\"first\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, getFirstAbsentIsNull) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"a=1\");\n"
        "String v = q.getFirst(\"nope\");\n"
        "return (v == null) ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, getAllAbsentIsEmptyArray) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"a=1\");\n"
        "String[] all = q.getAll(\"missing\");\n"
        "return (all.count() == 0) ? 1 : 0;")), 1);
}

// --- Bare flag (no '=') becomes (key, "") -------------------------------

TEST(UriQueryParamsTests, bareFlagHasEmptyValue) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"flag&x=1\");\n"
        "if (q.size() != 2) { return 0; }\n"
        "if (!q.keyAt(0).equals(\"flag\")) { return 0; }\n"
        "if (!q.valueAt(0).equals(\"\")) { return 0; }\n"
        "return q.keyAt(1).equals(\"x\") ? 1 : 0;")), 1);
}

// --- Value may itself contain '=' (split on FIRST '=') ------------------

TEST(UriQueryParamsTests, valueMayContainEquals) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"eq=a=b=c\");\n"
        "if (!q.keyAt(0).equals(\"eq\")) { return 0; }\n"
        "return q.valueAt(0).equals(\"a=b=c\") ? 1 : 0;")), 1);
}

// --- Empty segments (leading / trailing / doubled '&') skipped ----------

TEST(UriQueryParamsTests, emptySegmentsSkipped) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"&a=1&&b=2&\");\n"
        "if (q.size() != 2) { return 0; }\n"
        "if (!q.keyAt(0).equals(\"a\")) { return 0; }\n"
        "return q.keyAt(1).equals(\"b\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, nullQueryIsEmptyMap) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(null);\n"
        "return (q.size() == 0) ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, emptyQueryIsEmptyMap) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"\");\n"
        "return (q.size() == 0) ? 1 : 0;")), 1);
}

// --- Percent-decoding of keys and values --------------------------------

TEST(UriQueryParamsTests, parseDecodesPercentEscapes) {
    // %26 = '&', %3D = '=' — these are encoded in a value so they don't
    // mis-split, and must decode back to the literal chars.
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"k=a%26b%3Dc\");\n"
        "return q.valueAt(0).equals(\"a&b=c\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, parseDecodesUtf8) {
    // %C3%A9 = UTF-8 'é' (0xC3 0xA9).
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"name=%C3%A9\");\n"
        "String v = q.valueAt(0);\n"
        "if (v.byteLength() != 2) { return 0; }\n"
        "int32 b0 = ((int32) v.byteAt(0)) & 0xff;\n"
        "int32 b1 = ((int32) v.byteAt(1)) & 0xff;\n"
        "return (b0 == 0xC3 && b1 == 0xA9) ? 1 : 0;")), 1);
}

// --- Form mode: '+' decodes to space (the parse default) ----------------

TEST(UriQueryParamsTests, parsePlusDecodesToSpaceInFormMode) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"q=a+b+c\");\n"
        "return q.valueAt(0).equals(\"a b c\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, parsePercent2BDecodesToLiteralPlusInFormMode) {
    // %2B is a literal '+', distinct from the space-'+'.
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"q=a%2Bb\");\n"
        "return q.valueAt(0).equals(\"a+b\") ? 1 : 0;")), 1);
}

// --- Strict mode: '+' stays literal -------------------------------------

TEST(UriQueryParamsTests, parseStrictKeepsPlusLiteral) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parseStrict(\"q=a+b\");\n"
        "return q.valueAt(0).equals(\"a+b\") ? 1 : 0;")), 1);
}

// --- Malformed escape rejected (strict decoder) -------------------------

TEST(UriQueryParamsTests, parseRejectsTruncatedEscape) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    QueryParams q = QueryParams.parse(\"k=a%2\");\n"
        "    return 0;\n"
        "} catch (MalformedUriException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

// --- toString recompose --------------------------------------------------

TEST(UriQueryParamsTests, toStringJoinsPairs) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"x=1&y=2\");\n"
        "return q.toString().equals(\"x=1&y=2\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, toStringEmptyMapIsEmptyString) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"\");\n"
        "return q.toString().equals(\"\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, toStringEncodesSeparatorsInValue) {
    // A value containing '&' and '=' must be encoded so a later parse
    // can't mis-split it.
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = heap QueryParams();\n"
        "q.add(\"k\", \"a&b=c\");\n"
        "return q.toString().equals(\"k=a%26b%3Dc\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, toStringFormModeSpaceBecomesPlus) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = heap QueryParams();\n"
        "q.add(\"q\", \"a b\");\n"
        "return q.toString().equals(\"q=a+b\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, toStringFormModeLiteralPlusBecomesPercent2B) {
    // A literal '+' must encode to %2B (not '+', which means space).
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = heap QueryParams();\n"
        "q.add(\"q\", \"a+b\");\n"
        "return q.toString().equals(\"q=a%2Bb\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, toStringStrictModeSpaceBecomesPercent20) {
    // parseStrict keeps strict mode; a space toString-encodes to %20.
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parseStrict(\"k=v\");\n"
        "q.add(\"q\", \"a b\");\n"
        "return q.toString().equals(\"k=v&q=a%20b\") ? 1 : 0;")), 1);
}

// --- Round-trip: parse -> toString reproduces the (normalized) query ----

TEST(UriQueryParamsTests, roundTripFormMode) {
    // Spaces (as '+'), duplicate keys, encoded separators all survive.
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"a=1&a=2&q=x+y&v=p%26q\");\n"
        "return q.toString().equals(\"a=1&a=2&q=x+y&v=p%26q\") ? 1 : 0;")), 1);
}

// --- Mutators: add / set / remove ---------------------------------------

TEST(UriQueryParamsTests, setReplacesAllOccurrences) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"k=a&k=b&other=z\");\n"
        "q.set(\"k\", \"only\");\n"
        "if (q.countFor(\"k\") != 1) { return 0; }\n"
        "if (!q.getFirst(\"k\").equals(\"only\")) { return 0; }\n"
        "return q.contains(\"other\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, removeDropsEveryOccurrenceAndKeepsOrder) {
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = QueryParams.parse(\"a=1&b=2&a=3&c=4\");\n"
        "q.remove(\"a\");\n"
        "if (q.size() != 2) { return 0; }\n"
        "if (!q.keyAt(0).equals(\"b\")) { return 0; }\n"
        "return q.keyAt(1).equals(\"c\") ? 1 : 0;")), 1);
}

// --- Integration: Uri.queryParams() wires parse onto the raw query ------

TEST(UriQueryParamsTests, uriQueryParamsParsesRawQuery) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.parse(\"https://h.test/a/b?x=1&y=2&x=3\");\n"
        "QueryParams q = u.queryParams();\n"
        "if (q.size() != 3) { return 0; }\n"
        "if (q.countFor(\"x\") != 2) { return 0; }\n"
        "if (!q.getFirst(\"x\").equals(\"1\")) { return 0; }\n"
        "return q.getFirst(\"y\").equals(\"2\") ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, uriNoQueryYieldsEmptyParams) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.parse(\"https://h.test/a/b\");\n"
        "QueryParams q = u.queryParams();\n"
        "return (q.size() == 0) ? 1 : 0;")), 1);
}

TEST(UriQueryParamsTests, addGrowsBeyondInitialCapacity) {
    // Initial cap is 8; add 20 pairs to force at least two grows.
    EXPECT_EQ(runI32(makeSource(
        "QueryParams q = heap QueryParams();\n"
        "int32 i = 0;\n"
        "while (i < 20) {\n"
        "    q.add(\"k\", \"v\");\n"
        "    i = i + 1;\n"
        "}\n"
        "return (q.size() == 20 && q.countFor(\"k\") == 20) ? 1 : 0;")), 1);
}
