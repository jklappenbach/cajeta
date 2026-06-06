// Tests for cajeta.net.uri.MalformedUriException — plan item NET-6.5
// (`MalformedUri` reparented under `cajeta.net.NetException`).
//
// NET-6.1 originally rooted MalformedUriException on
// `cajeta.error.RecoverableException` as a placeholder, because the
// NetException base (NET-1.8) had not yet been built. NET-1.8 has now
// landed, so NET-6.5 reparents the class onto NetException. These tests
// pin the reparent contract:
//
//   1. a thrown MalformedUriException is catchable as the NetException
//      *root* (proves the `extends` chain), and
//   2. it carries the inherited `kind == NetException.KIND_INVALID` (12)
//      — a URI parse failure is the "invalid input" bucket, not an
//      errno-derived socket error, and
//   3. the NET-6.1 `(message, position)` contract is unchanged: the
//      byte-offset `position` still rides on the instance even when it
//      is caught at the base type, and
//   4. it is still catchable by the specific subtype (no regression to
//      the NET-6.1 UriParseTests catch(MalformedUriException) form).
//
// Harness mirrors UriParseTests / NetExceptionTests: compile a small
// cajeta source through the JIT, call run() -> int32.

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

// Wrap a method body in a class importing Uri, MalformedUriException and
// the NetException root. The body must `return` an int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.net.uri.Uri;\n"
           "import cajeta.net.uri.MalformedUriException;\n"
           "import cajeta.net.NetException;\n"
           "public final class U {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- (1) Reparent: catchable as the NetException root ------------------
// → NET-6.5 acceptance. An empty input throws MalformedUriException;
//   catching it as NetException proves the extends chain reaches the
//   networking root, not RecoverableException directly.

TEST(UriMalformedExceptionTests, caughtAsNetExceptionRoot) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u = Uri.parse(\"\");\n"
        "    return 0;\n"
        "} catch (NetException e) {\n"
        "    return 1;\n"
        "}")), 1);
}

// --- (2) Inherited kind discriminant -----------------------------------
// A malformed URI is classified to NetException.KIND_INVALID (12): a
// parse failure, not an errno-derived socket error. Read e.kind through
// the NetException base.

TEST(UriMalformedExceptionTests, carriesKindInvalidViaBase) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u = Uri.parse(\"http://[::1/path\");\n"
        "    return -1;\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"
        "}")), 12);
}

// Note: the kind is asserted as the literal ordinal 12 (mirroring
// `enum cajeta_net_err`), not `NetException.KIND_INVALID`, deliberately
// — NetExceptionTests.cpp keeps the same independence from cross-class
// static-constant resolution in this JIT harness. The named constant is
// the source-of-truth in MalformedUriException.cajeta itself.

// --- (3) position rides through the base catch -------------------------
// The NET-6.1 byte-offset contract survives the reparent: even caught
// at the subtype, position is the 0-based offset of the bad byte.
// "http://h.test:8o80/x" — the 'o' is at byte index 15 (see
// UriParseTests.malformedPortCitesPosition).

TEST(UriMalformedExceptionTests, positionPreservedAfterReparent) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u = Uri.parse(\"http://h.test:8o80/x\");\n"
        "    return -1;\n"
        "} catch (MalformedUriException e) {\n"
        "    return (int32) e.position;\n"
        "}")), 15);
}

// Empty / unpositioned input cites position -1 (failure not tied to a
// single byte) — and that field is intact post-reparent.

TEST(UriMalformedExceptionTests, emptyInputCitesNegativeOnePosition) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u = Uri.parse(\"\");\n"
        "    return -2;\n"
        "} catch (MalformedUriException e) {\n"
        "    return (int32) e.position;\n"
        "}")), -1);
}

// --- (4) Still catchable by the specific subtype -----------------------
// No regression: the NET-6.1 catch(MalformedUriException) form keeps
// working after the base class changed.

TEST(UriMalformedExceptionTests, stillCaughtBySpecificSubtype) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    Uri u = Uri.parse(\"http://h.test:99999/x\");\n"
        "    return 0;\n"
        "} catch (MalformedUriException e) {\n"
        "    return 1;\n"
        "}")), 1);
}
