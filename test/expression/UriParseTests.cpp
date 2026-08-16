// Tests for cajeta.io.net.uri.Uri — plan item NET-6.1 (RFC 3986 parse).
//
// Scope here is the component decomposition only: scheme / userinfo /
// host / port / path / query / fragment, IPv6 literal hosts, default
// ports per scheme, authority-less forms, and malformed-input
// rejection (MalformedUriException). Percent-encoding (NET-6.2),
// query-param multi-map (NET-6.3), and reference resolution
// (NET-6.4) are dependent items and are NOT exercised here.
//
// Harness mirrors PathTests / JsonReaderTests: compile a small cajeta
// source through the JIT, call run() -> int32. String-component checks
// use String.equals and return 1 on match / 0 otherwise; the test
// asserts EXPECT_EQ(runI32(...), 1).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Wrap a method body in a class importing Uri + String. The body must
// `return` an int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.uri.Uri;\n"
           "import cajeta.io.net.uri.MalformedUriException;\n"
           "public final class U {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- Full URI: all seven components ------------------------------------
// → UriTests.fullUriParsesAllComponents (plan acceptance)

TEST(UriParseTests, fullUriParsesScheme) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"https://u:p@h.test:8443/a/b?x=1&y=2#frag\");\n"
        "return u.getScheme().equals(\"https\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, fullUriParsesUserinfo) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"https://u:p@h.test:8443/a/b?x=1&y=2#frag\");\n"
        "return u.getUserinfo().equals(\"u:p\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, fullUriParsesHost) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"https://u:p@h.test:8443/a/b?x=1&y=2#frag\");\n"
        "return u.getHost().equals(\"h.test\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, fullUriParsesExplicitPort) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"https://u:p@h.test:8443/a/b?x=1&y=2#frag\");\n"
        "return u.getPort() == 8443 ? 1 : 0;")), 1);
}

TEST(UriParseTests, fullUriParsesPath) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"https://u:p@h.test:8443/a/b?x=1&y=2#frag\");\n"
        "return u.getPath().equals(\"/a/b\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, fullUriParsesQuery) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"https://u:p@h.test:8443/a/b?x=1&y=2#frag\");\n"
        "return u.getQuery().equals(\"x=1&y=2\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, fullUriParsesFragment) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"https://u:p@h.test:8443/a/b?x=1&y=2#frag\");\n"
        "return u.getFragment().equals(\"frag\") ? 1 : 0;")), 1);
}

// --- Default ports per scheme ------------------------------------------

TEST(UriParseTests, httpDefaultPort80) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"http://h.test/x\");\n"
        "return u.getPort() == 80 ? 1 : 0;")), 1);
}

TEST(UriParseTests, httpsDefaultPort443) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"https://h.test/x\");\n"
        "return u.getPort() == 443 ? 1 : 0;")), 1);
}

TEST(UriParseTests, wsDefaultPort80) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"ws://h.test/x\");\n"
        "return u.getPort() == 80 ? 1 : 0;")), 1);
}

TEST(UriParseTests, wssDefaultPort443) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"wss://h.test/x\");\n"
        "return u.getPort() == 443 ? 1 : 0;")), 1);
}

TEST(UriParseTests, explicitPortOverridesDefault) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"http://h.test:8080/x\");\n"
        "return u.getPort() == 8080 ? 1 : 0;")), 1);
}

// --- IPv6 literal authority --------------------------------------------
// → UriTests.ipv6LiteralAuthorityParses (plan acceptance)

TEST(UriParseTests, ipv6LiteralHostStripsBrackets) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"http://[::1]:80/\");\n"
        "return u.getHost().equals(\"::1\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, ipv6LiteralPort) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"http://[::1]:80/\");\n"
        "return u.getPort() == 80 ? 1 : 0;")), 1);
}

TEST(UriParseTests, ipv6LiteralNoExplicitPortUsesDefault) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"https://[2001:db8::1]/path\");\n"
        "return (u.getHost().equals(\"2001:db8::1\") && u.getPort() == 443) ? 1 : 0;")), 1);
}

// --- Authority-less forms ----------------------------------------------

TEST(UriParseTests, mailtoHasNoAuthority) {
    // mailto:a@b.test — no "//", so the whole "a@b.test" is the path.
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"mailto:a@b.test\");\n"
        "if (u.hasAuthority) { return 0; }\n"
        "if (!u.getScheme().equals(\"mailto\")) { return 0; }\n"
        "return u.getPath().equals(\"a@b.test\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, urnHasNoAuthority) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"urn:isbn:0451450523\");\n"
        "if (u.hasAuthority) { return 0; }\n"
        "if (!u.getScheme().equals(\"urn\")) { return 0; }\n"
        "return u.getPath().equals(\"isbn:0451450523\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, bareRelativePathNoScheme) {
    // "a/b/c" — no scheme (no ':' before first '/'), no authority.
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"a/b/c\");\n"
        "if (u.hasScheme) { return 0; }\n"
        "if (u.hasAuthority) { return 0; }\n"
        "return u.getPath().equals(\"a/b/c\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, pathWithColonSegmentIsNotScheme) {
    // "a/b:c" — the ':' is after a '/', so it is NOT a scheme delim.
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"a/b:c\");\n"
        "if (u.hasScheme) { return 0; }\n"
        "return u.getPath().equals(\"a/b:c\") ? 1 : 0;")), 1);
}

// --- Scheme case-insensitivity (RFC 3986 §3.1) -------------------------

TEST(UriParseTests, schemeLowercased) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"HTTPS://h.test/\");\n"
        "return u.getScheme().equals(\"https\") ? 1 : 0;")), 1);
}

// --- Empty / absent component distinctions -----------------------------

TEST(UriParseTests, emptyPathWhenAuthorityOnly) {
    // "http://h.test" — authority present, empty path.
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"http://h.test\");\n"
        "if (!u.hasAuthority) { return 0; }\n"
        "return u.getPath().equals(\"\") ? 1 : 0;")), 1);
}

TEST(UriParseTests, noQueryNoFragmentFlagsFalse) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"http://h.test/p\");\n"
        "if (u.hasQuery) { return 0; }\n"
        "if (u.hasFragment) { return 0; }\n"
        "return 1;")), 1);
}

TEST(UriParseTests, emptyQueryPresentButEmpty) {
    // "http://h.test/p?" — query present, empty string.
    EXPECT_EQ(runI32(makeSource(
        "Uri u #= Uri.parse(\"http://h.test/p?\");\n"
        "if (!u.hasQuery) { return 0; }\n"
        "return u.getQuery().equals(\"\") ? 1 : 0;")), 1);
}

// --- Malformed input rejection -----------------------------------------

TEST(UriParseTests, emptyInputRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u #= Uri.parse(\"\");\n"
        "    return 0;\n"
        "} catch (MalformedUriException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(UriParseTests, unterminatedIpv6LiteralRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u #= Uri.parse(\"http://[::1/path\");\n"
        "    return 0;\n"
        "} catch (MalformedUriException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(UriParseTests, nonNumericPortRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u #= Uri.parse(\"http://h.test:8o80/x\");\n"
        "    return 0;\n"
        "} catch (MalformedUriException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(UriParseTests, portOutOfRangeRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u #= Uri.parse(\"http://h.test:99999/x\");\n"
        "    return 0;\n"
        "} catch (MalformedUriException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

// --- Position citing on malformed input --------------------------------

TEST(UriParseTests, malformedPortCitesPosition) {
    // "http://h.test:8o80/x" — the 'o' is at byte index 15.
    //  h(7)t(8)t(9)p... actually count: "http://"=0..6, "h.test"=7..12,
    //  ':'=13, '8'=14, 'o'=15. The exception position should be 15.
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u #= Uri.parse(\"http://h.test:8o80/x\");\n"
        "    return -1;\n"
        "} catch (MalformedUriException e) {\n"
        "    return (int32) e.position;\n"
        "}")), 15);
}
