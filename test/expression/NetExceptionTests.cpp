// Tests for cajeta.net.NetException hierarchy + the cajeta_net_errno →
// exception mapping table — plan item NET-1.8.
//
// Scope: the *cajeta-side* error taxonomy that the NET-1.1 native errno
// shim feeds. NET-1.1 already collapses the platform errno
// (POSIX `errno` / Winsock `WSAGetLastError`) into a stable
// cross-platform ordinal (`enum cajeta_net_err`, returned by
// `__cajeta_net_last_error()`); those ordinals are pinned at the native
// level by NetSocketTests. NET-1.8 is the *second* half of the funnel:
// `NetErrors.fromErrno(ordinal, detail)` turning that ordinal into the
// typed NetException subtype, every subtype carrying its `kind` back.
//
// These are pure logic (no sockets) — golden-vector gtests over the JIT,
// the same harness UriParseTests / NetSocketTests' cajeta surface uses:
// compile a tiny cajeta source, call run() -> int32, assert the result.
//
// The ordinals here are literals mirroring `enum cajeta_net_err` in
// runtime/native/cajeta_net_socket.c (and NetException.KIND_*), exactly
// as NetSocketTests.cpp mirrors them with its own constexprs — keeping
// this test independent of cross-class static-constant resolution.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.N");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Wrap a method body in a class importing the net error types + String.
// The body must `return` an int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.net.NetException;\n"
           "import cajeta.net.NetErrors;\n"
           "import cajeta.net.ConnectionRefusedException;\n"
           "import cajeta.net.ConnectionResetException;\n"
           "import cajeta.net.ConnectionAbortedException;\n"
           "import cajeta.net.AddressInUseException;\n"
           "import cajeta.net.AddressNotAvailableException;\n"
           "import cajeta.net.HostUnreachableException;\n"
           "import cajeta.net.NetworkUnreachableException;\n"
           "import cajeta.net.BrokenPipeException;\n"
           "import cajeta.net.TimedOutException;\n"
           "import cajeta.net.WouldBlockException;\n"
           "public final class N {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

// Map an ordinal through NetErrors.fromErrno and return the `kind` of the
// produced exception. The whole point of the test: the returned subtype's
// own constructor stamped its `kind`, so reading it back proves the table
// selected the right concrete type for the ordinal.
std::string mapKind(int ordinal) {
    return "NetException e = NetErrors.fromErrno(" + std::to_string(ordinal) +
           ", \"detail\");\n"
           "return e.kind;";
}

}  // namespace

// --- Each ordinal maps to a subtype that stamps the matching kind --------
// Pins the full cajeta_net_errno → exception table (NET-1.8 deliverable).

TEST(NetExceptionTests, wouldBlockOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(1))), 1);
}
TEST(NetExceptionTests, connectionRefusedOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(2))), 2);
}
TEST(NetExceptionTests, connectionResetOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(3))), 3);
}
TEST(NetExceptionTests, connectionAbortedOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(4))), 4);
}
TEST(NetExceptionTests, addressInUseOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(5))), 5);
}
TEST(NetExceptionTests, addressNotAvailableOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(6))), 6);
}
TEST(NetExceptionTests, hostUnreachableOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(7))), 7);
}
TEST(NetExceptionTests, networkUnreachableOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(8))), 8);
}
TEST(NetExceptionTests, brokenPipeOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(9))), 9);
}
TEST(NetExceptionTests, timedOutOrdinalMapsToKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(10))), 10);
}

// --- Unmapped / OK ordinals fall through to base NetException ------------
// The fallback preserves the raw ordinal in `kind` so a caller can still
// discriminate; OTHER(99), INTERRUPTED(11), INVALID(12), ACCESS(13),
// IN_PROGRESS(14), and OK(0) all land here.

TEST(NetExceptionTests, otherOrdinalFallsThroughPreservingKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(99))), 99);
}
TEST(NetExceptionTests, interruptedOrdinalFallsThroughPreservingKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(11))), 11);
}
TEST(NetExceptionTests, accessOrdinalFallsThroughPreservingKind) {
    EXPECT_EQ(runI32(makeSource(mapKind(13))), 13);
}
TEST(NetExceptionTests, okOrdinalFallsThroughPreservingKind) {
    // OK is a caller bug, but it must not crash — it maps to base
    // NetException carrying kind 0, not null.
    EXPECT_EQ(runI32(makeSource(mapKind(0))), 0);
}

// --- The mapped exception is the right *runtime* type (typed catch) ------
// Beyond the kind tag: throwing a subtype and catching it by the specific
// subtype proves the dynamic type is correct, not merely the integer
// discriminant. Mirrors UriParseTests' catch(MalformedUriException). The
// throw uses the proven `throw heap Type(...)` form (no `throw <expr>`);
// the fromErrno→subtype selection is covered by the kind-read tests above.

TEST(NetExceptionTests, refusedCaughtBySpecificSubtype) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    throw heap ConnectionRefusedException(\"connect to 127.0.0.1:9\");\n"
        "} catch (ConnectionRefusedException e) {\n"
        "    return e.kind;\n"   // proves it is the refused subtype → 2
        "} catch (NetException other) {\n"
        "    return 0;\n"
        "}")), 2);
}

TEST(NetExceptionTests, timedOutCaughtBySpecificSubtype) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    throw heap TimedOutException(\"op deadline\");\n"
        "} catch (TimedOutException e) {\n"
        "    return e.kind;\n"   // → 10
        "} catch (NetException other) {\n"
        "    return 0;\n"
        "}")), 10);
}

// --- A subtype is catchable as the NetException root ----------------------
// The whole hierarchy hangs off NetException, so a broad `catch
// (NetException)` catches any subtype — the throws-clause discipline the
// error model relies on.

TEST(NetExceptionTests, subtypeCaughtAsNetExceptionRoot) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    throw heap AddressInUseException(\"bind 0.0.0.0:80\");\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"   // AddressInUse → 5
        "}")), 5);
}

// --- The detail message passes through untouched --------------------------
// NetErrors.fromErrno carries the caller-composed detail verbatim into the
// subtype, so the diagnostic (operation + cited address) survives.

TEST(NetExceptionTests, detailMessagePassesThrough) {
    EXPECT_EQ(runI32(makeSource(
        "NetException e = NetErrors.fromErrno(2, \"connect to 127.0.0.1:9\");\n"
        "return e.message.equals(\"connect to 127.0.0.1:9\") ? 1 : 0;")), 1);
}
