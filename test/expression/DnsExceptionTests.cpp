// Tests for the DNS exception taxonomy — plan item NET-2.5
// (`UnknownHostException` / `ResolutionFailedException` under
// `cajeta.io.net.NetException`, plus the `ResolveErrors.fromResolveErrno`
// ordinal → subtype mapper).
//
// NET-2.2 raised the *base* NetException on a failed lookup, carrying
// the raw resolve ordinal in `kind`, and explicitly deferred the typed
// narrowing to NET-2.5 (see DnsTests.badHostRaisesNetException). NET-2.5
// lands the two leaves and rewires Dns.resolve's failure path to throw
// them. These tests pin:
//
//   1. a NONAME lookup (an RFC 6761 `.invalid` name — a guaranteed
//      NXDOMAIN) now throws the *typed* UnknownHostException, still
//      catchable at the NetException root (no regression to NET-2.2's
//      universal net-out), and
//   2. it carries the precise resolve ordinal on `resolveErrno`
//      (1 = NONAME), while its coarse inherited `kind` is KIND_OTHER
//      (99) — DNS subtypes are discriminated by type, not errno, and
//   3. the reusable `ResolveErrors.fromResolveErrno` mapper is total and
//      splits the ordinal space correctly: NONAME/NODATA →
//      UnknownHostException, everything else (incl. the OK caller-bug
//      guard and any unknown ordinal) → ResolutionFailedException — a
//      golden-vector sweep over every ordinal, network-independent.
//
// (1)/(2) drive real getaddrinfo against a guaranteed-NXDOMAIN name, so
// they are deterministic offline. (3) is pure logic over the mapper, so
// the ResolutionFailed leaf and the full table are pinned without any
// network dependency at all.
//
// Harness mirrors DnsTests / UriMalformedExceptionTests: compile a small
// cajeta source through the JIT, call run() -> int32.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Wrap a method body in a class importing the DNS surface, the two new
// exception leaves, the mapper, and the NetException root. The body must
// `return` an int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.SocketAddress;\n"
           "import cajeta.io.net.NetException;\n"
           "import cajeta.io.net.dns.Dns;\n"
           "import cajeta.io.net.dns.ResolveErrors;\n"
           "import cajeta.io.net.dns.UnknownHostException;\n"
           "import cajeta.io.net.dns.ResolutionFailedException;\n"
           "public final class D {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// =====================================================================
//  (1) A NONAME lookup throws the typed UnknownHostException, still
//      catchable at the NetException root.
// =====================================================================

// `.invalid` is the RFC 6761 reserved TLD guaranteed never to resolve,
// so getaddrinfo returns EAI_NONAME (CAJETA_RESOLVE_NONAME) on every
// platform — a deterministic offline NXDOMAIN.

TEST(DnsExceptionTests, unknownHostThrownForNxdomain) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress[] addrs = Dns.resolve(\"no.such.host.cajeta.invalid\", 80);\n"
        "    return -1;\n"   // must not resolve
        "} catch (UnknownHostException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

// No regression to NET-2.2: the typed leaf is still caught by a
// `catch (NetException e)` — the universal request-boundary net-out.

TEST(DnsExceptionTests, unknownHostCaughtAsNetExceptionRoot) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress[] addrs = Dns.resolve(\"no.such.host.cajeta.invalid\", 80);\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

// =====================================================================
//  (2) The thrown UnknownHostException carries the precise resolve
//      ordinal (1 = NONAME) on resolveErrno; its coarse inherited kind
//      is KIND_OTHER (99).
// =====================================================================

TEST(DnsExceptionTests, unknownHostCarriesNonameResolveErrno) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress[] addrs = Dns.resolve(\"no.such.host.cajeta.invalid\", 80);\n"
        "    return -1;\n"
        "} catch (UnknownHostException e) {\n"
        "    return e.resolveErrno;\n"   // 1 = CAJETA_RESOLVE_NONAME
        "}")), 1);
}

TEST(DnsExceptionTests, unknownHostKindIsOtherViaBase) {
    // Read the inherited coarse `kind` through the NetException base:
    // DNS failures are discriminated by *type*, so kind is KIND_OTHER
    // (99), not the resolve ordinal.
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress[] addrs = Dns.resolve(\"no.such.host.cajeta.invalid\", 80);\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"   // 99 = KIND_OTHER
        "}")), 99);
}

// The failure message still names the host (the NET-2.2 host-citation
// contract survives the typed narrowing). The Dns failure message is
// "failed to resolve <host>"; assert its first byte is 'f' (102) so we
// confirm a non-empty host-naming message rode onto the typed leaf.

TEST(DnsExceptionTests, unknownHostMessageNamesHost) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress[] addrs = Dns.resolve(\"no.such.host.cajeta.invalid\", 80);\n"
        "    return -1;\n"
        "} catch (UnknownHostException e) {\n"
        "    String m = e.message;\n"
        "    if (m == null) { return -2; }\n"
        "    if (m.byteLength() < 1) { return -3; }\n"
        "    return ((int32) m.byteAt(0)) & 0xff;\n"   // 'f' == 102
        "}")), 102);
}

// =====================================================================
//  (3) ResolveErrors.fromResolveErrno — total ordinal → subtype mapper,
//      golden-vector sweep (network-independent).
//
//  NOTE on discrimination: `instanceof` in this JIT is a *compile-time*
//  static-type match (see TernaryAndInstanceOfTests), NOT a runtime RTTI
//  walk — so it cannot tell a NetException-typed handle's dynamic leaf
//  apart. The runtime *exception* path, by contrast, does a real
//  parent-vtable hierarchy walk (ErrorModelTests.uncaughtUnrecoverable*),
//  which is exactly what `catch (Subtype e)` dispatches on. So we
//  discriminate the mapper's result by **throwing it and catching by
//  subtype**: the most-specific matching catch that fires reveals the
//  dynamic type. `throw <local>` is the proven throw form
//  (ErrorModelTests lines 584/608).
// =====================================================================

// Helper body: feed `ord` to the mapper, throw the result, and report
// which subtype caught it — 1 = UnknownHost, 2 = ResolutionFailed.
// UnknownHost is caught first (most specific); since ResolutionFailed is
// a sibling, not a parent, an UnknownHost instance never lands in the
// ResolutionFailed catch and vice-versa.
static std::string classifyBody(int ord) {
    return
        "NetException e = ResolveErrors.fromResolveErrno(" + std::to_string(ord) + ", \"x\");\n"
        "try {\n"
        "    throw e;\n"
        "} catch (UnknownHostException u) {\n"
        "    return 1;\n"
        "} catch (ResolutionFailedException r) {\n"
        "    return 2;\n"
        "} catch (NetException n) {\n"
        "    return 3;\n"   // neither leaf — a totality failure
        "}";
}

// NONAME (1) and NODATA (2) → UnknownHostException.
TEST(DnsExceptionTests, mapperNonameIsUnknownHost) {
    EXPECT_EQ(runI32(makeSource(classifyBody(1))), 1);
}

TEST(DnsExceptionTests, mapperNodataIsUnknownHost) {
    EXPECT_EQ(runI32(makeSource(classifyBody(2))), 1);
}

// AGAIN (3), FAIL (4), FAMILY (5), MEMORY (6), SYSTEM (7), BADFLAGS (8),
// SERVICE (9), OTHER (99) → ResolutionFailedException. Golden sweep:
// each non-NONAME/NODATA ordinal maps to the failure leaf.
TEST(DnsExceptionTests, mapperAgainIsResolutionFailed) {
    EXPECT_EQ(runI32(makeSource(classifyBody(3))), 2);
}
TEST(DnsExceptionTests, mapperFailIsResolutionFailed) {
    EXPECT_EQ(runI32(makeSource(classifyBody(4))), 2);
}
TEST(DnsExceptionTests, mapperFamilyIsResolutionFailed) {
    EXPECT_EQ(runI32(makeSource(classifyBody(5))), 2);
}
TEST(DnsExceptionTests, mapperMemoryIsResolutionFailed) {
    EXPECT_EQ(runI32(makeSource(classifyBody(6))), 2);
}
TEST(DnsExceptionTests, mapperSystemIsResolutionFailed) {
    EXPECT_EQ(runI32(makeSource(classifyBody(7))), 2);
}
TEST(DnsExceptionTests, mapperBadflagsIsResolutionFailed) {
    EXPECT_EQ(runI32(makeSource(classifyBody(8))), 2);
}
TEST(DnsExceptionTests, mapperServiceIsResolutionFailed) {
    EXPECT_EQ(runI32(makeSource(classifyBody(9))), 2);
}
TEST(DnsExceptionTests, mapperOtherIsResolutionFailed) {
    EXPECT_EQ(runI32(makeSource(classifyBody(99))), 2);
}

// Totality guard: OK (0) is a caller bug — the resolve succeeded, so no
// exception should have been asked for. The mapper must still produce a
// non-null object (never null) in the failure bucket, surfacing the
// misuse loudly rather than NPE-ing later.
TEST(DnsExceptionTests, mapperOkOrdinalIsLoudResolutionFailure) {
    EXPECT_EQ(runI32(makeSource(
        "NetException e = ResolveErrors.fromResolveErrno(0, \"x\");\n"
        "if (e == null) { return -1; }\n"
        + std::string(
        "try {\n"
        "    throw e;\n"
        "} catch (ResolutionFailedException r) {\n"
        "    return 2;\n"
        "} catch (NetException n) {\n"
        "    return -2;\n"
        "}"))), 2);
}

// The mapper preserves the precise ordinal on the subtype's resolveErrno
// field (the coarse split never loses the fine cause — e.g. a retry
// policy can still single out AGAIN == 3). Read it through the caught
// subtype handle, whose static type carries the field.
TEST(DnsExceptionTests, mapperPreservesResolveErrnoOnFailureLeaf) {
    EXPECT_EQ(runI32(makeSource(
        "NetException e = ResolveErrors.fromResolveErrno(3, \"x\");\n"   // AGAIN
        "try {\n"
        "    throw e;\n"
        "} catch (ResolutionFailedException r) {\n"
        "    return r.resolveErrno;\n"
        "} catch (NetException n) {\n"
        "    return -1;\n"
        "}")), 3);
}

TEST(DnsExceptionTests, mapperPreservesResolveErrnoOnUnknownHostLeaf) {
    EXPECT_EQ(runI32(makeSource(
        "NetException e = ResolveErrors.fromResolveErrno(2, \"x\");\n"   // NODATA
        "try {\n"
        "    throw e;\n"
        "} catch (UnknownHostException u) {\n"
        "    return u.resolveErrno;\n"
        "} catch (NetException n) {\n"
        "    return -1;\n"
        "}")), 2);
}

// The named ordinal constants mirror the native enum exactly.
TEST(DnsExceptionTests, resolveOrdinalConstantsMatchNativeEnum) {
    EXPECT_EQ(runI32(makeSource(
        "if (ResolveErrors.RESOLVE_OK != 0) { return -1; }\n"
        "if (ResolveErrors.RESOLVE_NONAME != 1) { return -2; }\n"
        "if (ResolveErrors.RESOLVE_NODATA != 2) { return -3; }\n"
        "if (ResolveErrors.RESOLVE_AGAIN != 3) { return -4; }\n"
        "if (ResolveErrors.RESOLVE_FAIL != 4) { return -5; }\n"
        "if (ResolveErrors.RESOLVE_FAMILY != 5) { return -6; }\n"
        "if (ResolveErrors.RESOLVE_MEMORY != 6) { return -7; }\n"
        "if (ResolveErrors.RESOLVE_SYSTEM != 7) { return -8; }\n"
        "if (ResolveErrors.RESOLVE_BADFLAGS != 8) { return -9; }\n"
        "if (ResolveErrors.RESOLVE_SERVICE != 9) { return -10; }\n"
        "if (ResolveErrors.RESOLVE_OTHER != 99) { return -11; }\n"
        "return 1;")), 1);
}
