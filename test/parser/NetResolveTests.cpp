// NET-2.1 — native name-resolution intrinsics (`getaddrinfo`) tests.
//
// Exercises the native `__cajeta_net_getaddrinfo*` family through the JIT
// surface, the same way Sha256Tests drives `__cajeta_sha256_*`: a tiny
// `test.D` class declares the `@Native` bridges to the new symbols and the
// test asserts on the resolved address handle. NET-2.1 is the native syscall
// layer only; the pure-Cajeta `Dns.resolve` wrapper (NET-2.2) is intentionally
// not exercised here — these tests pin the intrinsic contract NET-2.2 builds on.
//
// The intrinsics live in runtime/native/cajeta_net_getaddrinfo.c (#included
// into cajeta_runtime.c after cajeta_net_socket.c so WSAStartup has run on
// Windows). The C core was independently cross-checked with a standalone
// compile against loopback (numeric v4, `localhost`, `::1`, a bad host) before
// these were committed.
//
// Pins NET-2.1 acceptance (the native half of the Phase-2 acceptance the
// NET-2.2 `DnsTests` later assert end-to-end):
//   - NetResolveTests.numericV4ResolvesToItself
//   - NetResolveTests.localhostResolvesToLoopback
//   - NetResolveTests.badHostReturnsNullWithNonameError
//   - NetResolveTests.familyFilterSelectsV6
//   - NetResolveTests.portIsBakedIntoEveryAddress

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// The shared `@Native` bridge preamble: every test snippet declares the same
// set of NET-2.1 intrinsics on its `test.D` class, then a `run()` drives them.
// `host` is emitted as an int8[] literal so arbitrary (possibly non-ASCII)
// host bytes round-trip exactly, mirroring the Sha256Tests data-literal shape.
const char* BRIDGES =
    "    @Native(\"__cajeta_net_getaddrinfo\")\n"
    "    public static #pointer resolve(int8[] host, int32 hostLen, int32 port, int32 family) { }\n"
    "    @Native(\"__cajeta_net_getaddrinfo_count\")\n"
    "    public static int32 count(pointer h) { }\n"
    "    @Native(\"__cajeta_net_getaddrinfo_error\")\n"
    "    public static int32 lastError() { }\n"
    "    @Native(\"__cajeta_net_getaddrinfo_family\")\n"
    "    public static int32 family(pointer h, int32 i) { }\n"
    "    @Native(\"__cajeta_net_getaddrinfo_port\")\n"
    "    public static int32 port(pointer h, int32 i) { }\n"
    "    @Native(\"__cajeta_net_getaddrinfo_octets\")\n"
    "    public static int32 octets(pointer h, int32 i, int8[] out) { }\n"
    "    @Native(\"__cajeta_net_freeaddrinfo\")\n"
    "    public static void release(pointer h) { }\n";

// Emit `host` as a sequence of int8[] element assignments into `name`.
std::string hostLiteral(const std::string& name, const std::string& host) {
    std::string s = "        int8[] " + name + " = new int8["
                  + std::to_string(host.size()) + "];\n";
    for (size_t i = 0; i < host.size(); i++) {
        s += "        " + name + "[" + std::to_string(i) + "L] = (int8) "
           + std::to_string((int)(signed char) host[i]) + ";\n";
    }
    return s;
}

// Compile + run an int32-returning `run()` body.
int32_t runInt(const std::string& body) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        + std::string(BRIDGES) +
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- numeric v4 resolves to itself -----------------------------------------

TEST(NetResolveTests, numericV4ResolvesToItself) {
    // "127.0.0.1" with the AF_UNSPEC (both) filter resolves to exactly one
    // V4 address whose octets are 127.0.0.1 and whose port is the one asked.
    // Returns a bit-packed result: 1 iff family==V4(0), count>=1, octets
    // match, port==8080, and the error ordinal is OK(0).
    std::string body =
        hostLiteral("host", "127.0.0.1") +
        "        pointer h = resolve(host, 9, 8080, -1);\n"
        "        int32 err = lastError();\n"
        "        int32 c = count(h);\n"
        "        int32 fam = family(h, 0);\n"
        "        int32 p = port(h, 0);\n"
        "        int8[] oct = new int8[16];\n"
        "        int32 w = octets(h, 0, oct);\n"
        "        int32 o0 = ((int32) oct[0L]) & 0xff;\n"
        "        int32 o1 = ((int32) oct[1L]) & 0xff;\n"
        "        int32 o2 = ((int32) oct[2L]) & 0xff;\n"
        "        int32 o3 = ((int32) oct[3L]) & 0xff;\n"
        "        release(h);\n"
        "        if (err != 0) { return -1; }\n"
        "        if (c < 1) { return -2; }\n"
        "        if (fam != 0) { return -3; }\n"
        "        if (w != 4) { return -4; }\n"
        "        if (o0 != 127 || o1 != 0 || o2 != 0 || o3 != 1) { return -5; }\n"
        "        if (p != 8080) { return -6; }\n"
        "        return 1;\n";
    EXPECT_EQ(runInt(body), 1);
}

// --- localhost resolves to a loopback address -------------------------------

TEST(NetResolveTests, localhostResolvesToLoopback) {
    // `localhost` resolves to at least one address; at least one of them is a
    // loopback (V4 127.0.0.1 or V6 ::1). Mirrors DnsTests.localhostResolvesToLoopback
    // at the native layer.
    std::string body =
        hostLiteral("host", "localhost") +
        "        pointer h = resolve(host, 9, 443, -1);\n"
        "        int32 c = count(h);\n"
        "        int32 sawLoopback = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < c) {\n"
        "            int32 fam = family(h, i);\n"
        "            int8[] oct = new int8[16];\n"
        "            int32 w = octets(h, i, oct);\n"
        "            if (fam == 0 && w == 4) {\n"
        "                int32 a0 = ((int32) oct[0L]) & 0xff;\n"
        "                int32 a3 = ((int32) oct[3L]) & 0xff;\n"
        "                if (a0 == 127 && a3 == 1) { sawLoopback = 1; }\n"
        "            }\n"
        "            if (fam == 1 && w == 16) {\n"
        "                int32 last = ((int32) oct[15L]) & 0xff;\n"
        "                if (last == 1) { sawLoopback = 1; }\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        release(h);\n"
        "        if (c < 1) { return -2; }\n"
        "        return sawLoopback;\n";
    EXPECT_EQ(runInt(body), 1);
}

// --- a bad host returns NULL with the NONAME error --------------------------

TEST(NetResolveTests, badHostReturnsNullWithNonameError) {
    // An unresolvable name returns a NULL handle and sets the normalized
    // resolve error to NONAME (ordinal 1) — the ordinal NET-2.5's `Dns` layer
    // maps to `UnknownHost`. Mirrors DnsTests.badHostRaisesUnknownHost.
    std::string body =
        hostLiteral("host", "no.such.host.cajeta.invalid") +
        "        pointer h = resolve(host, 27, 80, -1);\n"
        "        int32 err = lastError();\n"
        // A NULL handle reports count 0 (every accessor is NULL-safe); a
        // resolved handle would report >= 1. Release is NULL-safe either way.
        "        int32 c = count(h);\n"
        "        release(h);\n"
        "        if (c != 0) { return -1; }\n"
        "        if (err != 1) { return -2; }\n"   // CAJETA_RESOLVE_NONAME
        "        return 1;\n";
    EXPECT_EQ(runInt(body), 1);
}

// --- family filter selects only the requested family ------------------------

TEST(NetResolveTests, familyFilterSelectsV6) {
    // Resolving the numeric "::1" with the V6-only filter (1) yields a single
    // V6 address whose 16 octets are ::1 (last byte 1, all others 0).
    std::string body =
        hostLiteral("host", "::1") +
        "        pointer h = resolve(host, 3, 80, 1);\n"
        "        int32 c = count(h);\n"
        "        int32 fam = family(h, 0);\n"
        "        int8[] oct = new int8[16];\n"
        "        int32 w = octets(h, 0, oct);\n"
        "        int32 sum = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 15) {\n"
        "            sum = sum + (((int32) oct[(int64) i]) & 0xff);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 last = ((int32) oct[15L]) & 0xff;\n"
        "        release(h);\n"
        "        if (c < 1) { return -2; }\n"
        "        if (fam != 1) { return -3; }\n"
        "        if (w != 16) { return -4; }\n"
        "        if (sum != 0) { return -5; }\n"   // leading 15 bytes all zero
        "        if (last != 1) { return -6; }\n"
        "        return 1;\n";
    EXPECT_EQ(runInt(body), 1);
}

// --- the requested port is baked into every resolved address ----------------

TEST(NetResolveTests, portIsBakedIntoEveryAddress) {
    // Every address returned for `localhost` carries the requested port (the
    // port `getaddrinfo` baked into the sockaddr, surfaced host-order).
    std::string body =
        hostLiteral("host", "localhost") +
        "        pointer h = resolve(host, 9, 9090, -1);\n"
        "        int32 c = count(h);\n"
        "        int32 ok = 1;\n"
        "        int32 i = 0;\n"
        "        while (i < c) {\n"
        "            if (port(h, i) != 9090) { ok = 0; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        release(h);\n"
        "        if (c < 1) { return -2; }\n"
        "        return ok;\n";
    EXPECT_EQ(runInt(body), 1);
}

// --- out-of-range index returns the defensive sentinel ----------------------

TEST(NetResolveTests, outOfRangeIndexReturnsSentinel) {
    // Past-the-end accessor calls return -1 rather than faulting (the cajeta
    // iteration is 0..count-1, but a defensive sentinel beats UB on a bug).
    std::string body =
        hostLiteral("host", "127.0.0.1") +
        "        pointer h = resolve(host, 9, 80, -1);\n"
        "        int32 c = count(h);\n"
        "        int8[] oct = new int8[16];\n"
        "        int32 fam = family(h, c);\n"     // index == count → OOR
        "        int32 p = port(h, c);\n"
        "        int32 w = octets(h, c, oct);\n"
        "        release(h);\n"
        "        if (fam != -1) { return -1; }\n"
        "        if (p != -1) { return -2; }\n"
        "        if (w != -1) { return -3; }\n"
        "        return 1;\n";
    EXPECT_EQ(runInt(body), 1);
}
