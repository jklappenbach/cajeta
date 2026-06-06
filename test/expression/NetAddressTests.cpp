// Tests for cajeta.net.IpAddress + cajeta.net.SocketAddress —
// plan item NET-1.2 (address types: parse + canonical format).
//
// Scope: IPv4 dotted-quad + IPv6 literal parsing (incl. "::" zero-run
// compression and the embedded-IPv4 tail), the bracketed "[v6]:port"
// SocketAddress form, canonical RFC 5952 IPv6 output, byte-identical
// parse->toString round-tripping, and malformed-input rejection
// (MalformedAddressException). The native sockaddr pack/unpack
// intrinsics are pinned separately by NetSockaddrTests.
//
// Harness mirrors UriParseTests / PathTests: compile a small cajeta
// source through the JIT, call run() -> int32. The body returns 1 on
// the expected result and 0 otherwise; the test asserts
// EXPECT_EQ(runI32(...), 1).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Wrap a method body in a class importing the address types + String.
// The body must `return` an int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.net.IpAddress;\n"
           "import cajeta.net.SocketAddress;\n"
           "import cajeta.net.AddressFamily;\n"
           "import cajeta.net.MalformedAddressException;\n"
           "public final class A {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// ====================== IpAddress — IPv4 ===================================

TEST(NetAddressTests, v4ParseFamilyIsV4) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"127.0.0.1\");\n"
        "return a.isV4() ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v4ParseOctets) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"192.168.1.250\");\n"
        "int8[] o = a.getOctets();\n"
        "int32 b0 = ((int32) o[0]) & 0xff;\n"
        "int32 b1 = ((int32) o[1]) & 0xff;\n"
        "int32 b2 = ((int32) o[2]) & 0xff;\n"
        "int32 b3 = ((int32) o[3]) & 0xff;\n"
        "return (b0 == 192 && b1 == 168 && b2 == 1 && b3 == 250) ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v4RoundTrip) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"203.0.113.7\");\n"
        "return a.toString().equals(\"203.0.113.7\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v4AllZeros) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"0.0.0.0\");\n"
        "return a.toString().equals(\"0.0.0.0\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v4Broadcast) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"255.255.255.255\");\n"
        "return a.toString().equals(\"255.255.255.255\") ? 1 : 0;")), 1);
}

// ====================== IpAddress — IPv6 ===================================

TEST(NetAddressTests, v6ParseFamilyIsV6) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"::1\");\n"
        "return a.isV6() ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v6LoopbackRoundTrip) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"::1\");\n"
        "return a.toString().equals(\"::1\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v6UnspecifiedRoundTrip) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"::\");\n"
        "return a.toString().equals(\"::\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v6FullRoundTrip) {
    // Already canonical: lowercase, no compressible run.
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"2001:db8:1:2:3:4:5:6\");\n"
        "return a.toString().equals(\"2001:db8:1:2:3:4:5:6\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v6MidRunCompressed) {
    // 2001:db8:0:0:0:0:0:1 canonicalizes to 2001:db8::1.
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"2001:db8::1\");\n"
        "return a.toString().equals(\"2001:db8::1\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v6LeadingZerosStripped) {
    // 2001:0db8:0000:0000:0000:0000:0000:0001 -> 2001:db8::1
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"2001:0db8:0000:0000:0000:0000:0000:0001\");\n"
        "return a.toString().equals(\"2001:db8::1\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v6LongestRunChosen) {
    // 1:0:0:1:0:0:0:1 -> the longer (second) run collapses: 1:0:0:1::1
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"1:0:0:1:0:0:0:1\");\n"
        "return a.toString().equals(\"1:0:0:1::1\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v6TrailingRunCompressed) {
    // fe80:0:0:0:0:0:0:0 -> fe80::
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"fe80::\");\n"
        "return a.toString().equals(\"fe80::\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v6UppercaseInputLowercasedOutput) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"2001:DB8::AB\");\n"
        "return a.toString().equals(\"2001:db8::ab\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, v6EmbeddedV4Octets) {
    // ::ffff:127.0.0.1 -> octets[10]=0xff octets[11]=0xff octets[12..16)=127.0.0.1
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"::ffff:127.0.0.1\");\n"
        "int8[] o = a.getOctets();\n"
        "int32 b10 = ((int32) o[10]) & 0xff;\n"
        "int32 b11 = ((int32) o[11]) & 0xff;\n"
        "int32 b12 = ((int32) o[12]) & 0xff;\n"
        "int32 b15 = ((int32) o[15]) & 0xff;\n"
        "return (b10 == 0xff && b11 == 0xff && b12 == 127 && b15 == 1) ? 1 : 0;")), 1);
}

// ====================== IpAddress — factories + equality ===================

TEST(NetAddressTests, loopbackV4Factory) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.loopbackV4();\n"
        "return a.toString().equals(\"127.0.0.1\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, loopbackV6Factory) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.loopbackV6();\n"
        "return a.toString().equals(\"::1\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, equalsSameV4) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"10.0.0.1\");\n"
        "IpAddress b = IpAddress.parse(\"10.0.0.1\");\n"
        "return a.equals(b) ? 1 : 0;")), 1);
}

TEST(NetAddressTests, notEqualsAcrossFamilies) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress a = IpAddress.parse(\"0.0.0.0\");\n"
        "IpAddress b = IpAddress.parse(\"::\");\n"
        "return a.equals(b) ? 0 : 1;")), 1);
}

// ====================== IpAddress — malformed ==============================

TEST(NetAddressTests, v4OctetOutOfRangeRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"256.0.0.1\");\n"
        "    return 0;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(NetAddressTests, v4TooFewOctetsRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"1.2.3\");\n"
        "    return 0;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(NetAddressTests, v6DoubleColonColonRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"1::2::3\");\n"
        "    return 0;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(NetAddressTests, emptyInputRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"\");\n"
        "    return 0;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

// ====================== SocketAddress ======================================

TEST(NetAddressTests, socketV4Parse) {
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress s = SocketAddress.parse(\"127.0.0.1:8080\");\n"
        "if (s.getPort() != 8080) { return 0; }\n"
        "return s.getIp().toString().equals(\"127.0.0.1\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, socketV4RoundTrip) {
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress s = SocketAddress.parse(\"203.0.113.7:443\");\n"
        "return s.toString().equals(\"203.0.113.7:443\") ? 1 : 0;")), 1);
}

// → NetAddressTests.v4AndV6ParseRoundTrip (plan acceptance)
TEST(NetAddressTests, v4AndV6ParseRoundTrip) {
    // The named acceptance check: both families parse + reformat
    // byte-identically, including the [::1]:8080 bracket form.
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress v4 = SocketAddress.parse(\"127.0.0.1:8080\");\n"
        "if (!v4.toString().equals(\"127.0.0.1:8080\")) { return 0; }\n"
        "SocketAddress v6 = SocketAddress.parse(\"[::1]:8080\");\n"
        "if (!v6.toString().equals(\"[::1]:8080\")) { return 0; }\n"
        "return 1;")), 1);
}

TEST(NetAddressTests, socketV6BracketParse) {
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress s = SocketAddress.parse(\"[2001:db8::1]:443\");\n"
        "if (s.getPort() != 443) { return 0; }\n"
        "return s.getIp().toString().equals(\"2001:db8::1\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, socketV6BracketRoundTrip) {
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress s = SocketAddress.parse(\"[2001:db8::1]:443\");\n"
        "return s.toString().equals(\"[2001:db8::1]:443\") ? 1 : 0;")), 1);
}

TEST(NetAddressTests, socketPortZeroRoundTrip) {
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress s = SocketAddress.parse(\"0.0.0.0:0\");\n"
        "return s.toString().equals(\"0.0.0.0:0\") ? 1 : 0;")), 1);
}

// ====================== SocketAddress — malformed ==========================

TEST(NetAddressTests, socketUnbracketedV6Rejected) {
    // "::1:8080" is an unbracketed IPv6 (multiple colons) — must be
    // rejected; the user has to write [::1]:8080.
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress s = SocketAddress.parse(\"::1:8080\");\n"
        "    return 0;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(NetAddressTests, socketMissingPortRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress s = SocketAddress.parse(\"127.0.0.1\");\n"
        "    return 0;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(NetAddressTests, socketPortOutOfRangeRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress s = SocketAddress.parse(\"127.0.0.1:99999\");\n"
        "    return 0;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

TEST(NetAddressTests, socketUnterminatedBracketRejected) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress s = SocketAddress.parse(\"[::1:8080\");\n"
        "    return 0;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 1;\n"
        "}")), 1);
}
