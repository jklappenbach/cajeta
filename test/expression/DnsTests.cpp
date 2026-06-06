// Tests for cajeta.net.dns.Dns + cajeta.net.dns.ResolveFamily —
// plan item NET-2.2 (blocking name resolution).
//
// Scope: the pure-Cajeta `Dns.resolve` wrapper over the NET-2.1
// `__cajeta_net_getaddrinfo*` intrinsics — the three `resolve`
// overloads (host; host+port; host+port+family), the `ResolveFamily`
// → native-filter mapping, per-entry unmarshalling into
// SocketAddress, port baking, and the failure → NetException raise
// (NET-2.5 later narrows the base NetException to UnknownHost /
// ResolutionFailed; NET-2.2 only owns the base raise + host citation).
//
// The NET-2.1 *native* layer is pinned independently by
// NetResolveTests (test/parser/NetResolveTests.cpp); these tests pin
// the NET-2.2 *Cajeta* surface that wraps it, so they assert on
// SocketAddress / IpAddress objects rather than raw octet accessors.
//
// Harness mirrors NetAddressTests: compile a small cajeta source
// through the JIT, call run() -> int32; the body returns 1 on the
// expected result and 0 (or a negative diagnostic) otherwise.
//
// These pin the NET-2.2 slice of the Phase-2 acceptance:
//   - DnsTests.localhostResolvesToLoopback   (plan: Dns.resolve("localhost") → loopback)
//   - DnsTests.badHostRaisesNetException     (plan: bad host raises, naming host — base form; NET-2.5 narrows)
//   - DnsTests.numericV4ResolvesToItself
//   - DnsTests.portIsBakedIntoEveryAddress
//   - DnsTests.v6OnlyFilterSelectsV6
//   - DnsTests.v4OnlyFilterExcludesV6

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

// Wrap a method body in a class importing the DNS surface + the
// address types it returns. The body must `return` an int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.net.IpAddress;\n"
           "import cajeta.net.SocketAddress;\n"
           "import cajeta.net.AddressFamily;\n"
           "import cajeta.net.NetException;\n"
           "import cajeta.net.dns.Dns;\n"
           "import cajeta.net.dns.ResolveFamily;\n"
           "public final class A {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// ====================== resolve("localhost") ==============================

TEST(DnsTests, localhostResolvesToLoopback) {
    // `localhost` resolves to >= 1 address, at least one of which is a
    // loopback (V4 127.0.0.1 or V6 ::1). Drives Dns.resolve(host, port)
    // and inspects the returned SocketAddress[] through the Cajeta
    // IpAddress accessors (not raw octet intrinsics).
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress[] addrs = Dns.resolve(\"localhost\", 443);\n"
        "int32 c = addrs.count();\n"
        "if (c < 1) { return -1; }\n"
        "int32 sawLoopback = 0;\n"
        "int32 i = 0;\n"
        "while (i < c) {\n"
        "    IpAddress ip = addrs[i].getIp();\n"
        "    int8[] o = ip.getOctets();\n"
        "    if (ip.isV4()) {\n"
        "        int32 a0 = ((int32) o[0]) & 0xff;\n"
        "        int32 a3 = ((int32) o[3]) & 0xff;\n"
        "        if (a0 == 127 && a3 == 1) { sawLoopback = 1; }\n"
        "    } else {\n"
        "        int32 last = ((int32) o[15]) & 0xff;\n"
        "        int32 sum = 0;\n"
        "        int32 j = 0;\n"
        "        while (j < 15) { sum = sum + (((int32) o[(int64) j]) & 0xff); j = j + 1; }\n"
        "        if (sum == 0 && last == 1) { sawLoopback = 1; }\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return sawLoopback;")), 1);
}

// ====================== numeric v4 resolves to itself =====================

TEST(DnsTests, numericV4ResolvesToItself) {
    // A numeric literal resolves (via getaddrinfo's AI_NUMERICSERV +
    // numeric-host fast path) to exactly that address.
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress[] addrs = Dns.resolve(\"127.0.0.1\", 8080);\n"
        "if (addrs.count() < 1) { return -1; }\n"
        "SocketAddress a = addrs[0];\n"
        "IpAddress ip = a.getIp();\n"
        "if (!ip.isV4()) { return -2; }\n"
        "int8[] o = ip.getOctets();\n"
        "int32 o0 = ((int32) o[0]) & 0xff;\n"
        "int32 o3 = ((int32) o[3]) & 0xff;\n"
        "if (o0 != 127 || o3 != 1) { return -3; }\n"
        "if (a.getPort() != 8080) { return -4; }\n"
        "return 1;")), 1);
}

// ====================== the port is baked into every address ==============

TEST(DnsTests, portIsBakedIntoEveryAddress) {
    // Every SocketAddress in the result carries the requested port.
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress[] addrs = Dns.resolve(\"localhost\", 9090);\n"
        "int32 c = addrs.count();\n"
        "if (c < 1) { return -1; }\n"
        "int32 i = 0;\n"
        "while (i < c) {\n"
        "    if (addrs[i].getPort() != 9090) { return -2; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return 1;")), 1);
}

// ====================== the single-arg overload defaults port 0 ===========

TEST(DnsTests, singleArgOverloadDefaultsPortZero) {
    // Dns.resolve(host) bakes port 0 and applies no family restriction.
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress[] addrs = Dns.resolve(\"127.0.0.1\");\n"
        "if (addrs.count() < 1) { return -1; }\n"
        "return (addrs[0].getPort() == 0) ? 1 : 0;")), 1);
}

// ====================== the V6-only filter selects V6 =====================

TEST(DnsTests, v6OnlyFilterSelectsV6) {
    // Resolving the numeric "::1" with V6_ONLY yields a single V6
    // address (::1).
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress[] addrs = Dns.resolve(\"::1\", 80, ResolveFamily.V6_ONLY);\n"
        "if (addrs.count() < 1) { return -1; }\n"
        "IpAddress ip = addrs[0].getIp();\n"
        "if (!ip.isV6()) { return -2; }\n"
        "int8[] o = ip.getOctets();\n"
        "int32 last = ((int32) o[15]) & 0xff;\n"
        "int32 sum = 0;\n"
        "int32 j = 0;\n"
        "while (j < 15) { sum = sum + (((int32) o[(int64) j]) & 0xff); j = j + 1; }\n"
        "if (sum != 0) { return -3; }\n"
        "return (last == 1) ? 1 : 0;")), 1);
}

// ====================== the V4-only filter excludes V6 ====================

TEST(DnsTests, v4OnlyFilterReturnsOnlyV4) {
    // localhost under V4_ONLY returns only V4 addresses (no V6 in the set).
    EXPECT_EQ(runI32(makeSource(
        "SocketAddress[] addrs = Dns.resolve(\"localhost\", 80, ResolveFamily.V4_ONLY);\n"
        "int32 c = addrs.count();\n"
        "if (c < 1) { return -1; }\n"
        "int32 i = 0;\n"
        "while (i < c) {\n"
        "    if (!addrs[i].getIp().isV4()) { return -2; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return 1;")), 1);
}

// ====================== a bad host raises a NetException ===================

TEST(DnsTests, badHostRaisesNetException) {
    // An unresolvable name raises (NET-2.2 raises the base NetException;
    // NET-2.5 will narrow it to UnknownHost). The raise is catchable at
    // the NetException root — the universal net-out — and the resolve
    // ordinal is carried in `kind` (1 = NONAME for a not-found name).
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress[] addrs = Dns.resolve(\"no.such.host.cajeta.invalid\", 80);\n"
        "    return -1;\n"   // should not reach here
        "} catch (NetException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

// ====================== a null host raises before the syscall =============

TEST(DnsTests, nullHostRaisesNetException) {
    // A null host is rejected up front with KIND_INVALID (12), never
    // reaching the native layer.
    EXPECT_EQ(runI32(makeSource(
        "String h = null;\n"
        "try {\n"
        "    SocketAddress[] addrs = Dns.resolve(h, 80);\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return (e.kind == NetException.KIND_INVALID) ? 1 : 0;\n"
        "}")), 1);
}
