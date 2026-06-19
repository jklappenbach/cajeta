// Tests for the consolidated cajeta.io.net error taxonomy — plan item
// NET-11.6 ("NetException root + the shared error taxonomy doc; every
// phase's exceptions hang off it").
//
// NET-1.8 built the socket-layer subtypes + the errno→exception map
// (pinned by NetExceptionTests.cpp). NET-6.5 reparented the URI parse
// failure onto NetException (pinned by UriMalformedExceptionTests.cpp).
// NET-11.6 is the *consolidation*: the invariant that the WHOLE tree —
// across packages (cajeta.io.net + cajeta.io.net.uri) — descends from the one
// `cajeta.io.net.NetException` root, so a single request-boundary
// `catch (NetException e)` is the universal net-out for any networking
// failure regardless of which layer raised it.
//
// This file owns the two facts NET-11.6 adds that no sibling test
// covers:
//   (A) MalformedAddressException (NET-1.2) was reparented onto
//       NetException as part of the consolidation — it is catchable as
//       the root and carries the inherited kind == KIND_INVALID (12),
//       exactly like its MalformedUriException peer; and
//   (B) the cross-package single-root invariant: a socket-layer
//       subtype, a URI parse failure, and an address parse failure are
//       all catchable by one `catch (NetException)` — the property the
//       taxonomy doc (docs/specification/io/net/Errors.md) promises.
//
// Pure logic, no sockets — the same golden JIT harness NetExceptionTests
// / UriMalformedExceptionTests use: compile a tiny cajeta source, call
// run() -> int32, assert the result. Ordinals are asserted as literals
// (mirroring `enum cajeta_net_err` / NetException.KIND_*), matching the
// independence-from-cross-class-static-resolution convention the sibling
// tests keep.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Wrap a body in a class importing the whole taxonomy across both
// packages: the NetException root, the socket subtypes used here, the
// address parse types, and the URI parse types. The body returns int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.NetException;\n"
           "import cajeta.io.net.ConnectionRefusedException;\n"
           "import cajeta.io.net.MalformedAddressException;\n"
           "import cajeta.io.net.IpAddress;\n"
           "import cajeta.io.net.SocketAddress;\n"
           "import cajeta.io.net.uri.Uri;\n"
           "import cajeta.io.net.uri.MalformedUriException;\n"
           "public final class T {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

}  // namespace

// --- (A) MalformedAddressException reparent ------------------------------
// A malformed IP address throws MalformedAddressException; catching it as
// the NetException root proves the `extends` chain now reaches the
// networking root (NET-11.6 consolidation), not RecoverableException.

TEST(NetTaxonomyTests, malformedAddressCaughtAsNetExceptionRoot) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"garbage\");\n"
        "    return 0;\n"
        "} catch (NetException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

// It carries the inherited kind == KIND_INVALID (12) read through the
// base — a parse failure, the same "invalid input" bucket the URI parse
// failure uses, not an errno-derived socket error.

TEST(NetTaxonomyTests, malformedAddressCarriesKindInvalidViaBase) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"999.0.0.0\");\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"
        "}")), 12);
}

// The NET-1.2 `(message, position)` contract survives the reparent: the
// byte-offset `position` still rides on the instance even when caught at
// the base type. "999.0.0.0" — the octet overflows at byte index 2.

TEST(NetTaxonomyTests, malformedAddressPositionPreservedAfterReparent) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"999.0.0.0\");\n"
        "    return -1;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return (int32) e.position;\n"
        "}")), 2);
}

// Empty / unpositioned input cites position -1 (failure not tied to a
// single byte) — intact post-reparent.

TEST(NetTaxonomyTests, malformedAddressEmptyCitesNegativeOnePosition) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"\");\n"
        "    return -2;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return (int32) e.position;\n"
        "}")), -1);
}

// No regression: still catchable by the specific subtype after the base
// class changed.

TEST(NetTaxonomyTests, malformedAddressStillCaughtBySpecificSubtype) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    SocketAddress s = SocketAddress.parse(\"127.0.0.1\");\n"  // no :port
        "    return 0;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

// --- (B) Cross-package single-root invariant -----------------------------
// The whole point of NET-11.6: a single `catch (NetException)` catches a
// failure from ANY layer/package. Each of the three returns a distinct
// non-zero code, proving the catch fired for that source (and that none
// escaped uncaught past the root).

// A socket-layer subtype (cajeta.io.net) — thrown directly via the proven
// `throw heap` form.
TEST(NetTaxonomyTests, socketSubtypeCaughtAtRoot) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    throw heap ConnectionRefusedException(\"connect to 127.0.0.1:9\");\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"   // ConnectionRefused → 2
        "}")), 2);
}

// An address parse failure (cajeta.io.net) — caught at the same root.
TEST(NetTaxonomyTests, addressFailureCaughtAtRoot) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"not-an-ip\");\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"   // parse failure → KIND_INVALID 12
        "}")), 12);
}

// A URI parse failure (cajeta.io.net.uri — a *different package*) — caught
// at the same root, proving the tree spans packages.
TEST(NetTaxonomyTests, uriFailureFromOtherPackageCaughtAtRoot) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u = Uri.parse(\"\");\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"   // URI parse failure → KIND_INVALID 12
        "}")), 12);
}

// And the first-match-wins ordering holds: a more-specific catch ahead of
// the root binds the subtype; the root only catches what the specific arm
// missed. This is the property the catch-granularity guidance in
// Errors.md documents.
TEST(NetTaxonomyTests, specificArmBindsBeforeRootArm) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    IpAddress a = IpAddress.parse(\"garbage\");\n"
        "    return -1;\n"
        "} catch (MalformedAddressException e) {\n"
        "    return 7;\n"        // specific arm wins
        "} catch (NetException e) {\n"
        "    return 0;\n"        // must NOT reach the root arm
        "}")), 7);
}
