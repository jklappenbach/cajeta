// Tests for cajeta.net.TcpStream's sequential v4/v6 connect fallback —
// plan item NET-2.6 (`TcpStream.connect(host, port)` resolves via the
// blocking NET-2.2 resolver, then tries the resolved addresses in
// order until one connects; happy-eyeballs parallel racing is deferred
// to NET-12.6).
//
// Harness mirrors DnsTests (test/expression/DnsTests.cpp): compile a
// small cajeta source through the JIT, call run() -> int32; the body
// returns 1 on the expected result and 0 (or a negative diagnostic)
// otherwise. The Cajeta-surface fallback logic under test lives in
// runtime/src/cajeta/net/TcpStream.cajeta (connect(String,int32) +
// the private connectAny core), built on Dns.resolve (NET-2.2, a
// working @Native bridge) and TcpStream.connect(#SocketAddress)
// (NET-1.3).
//
// =====================================================================
// IMPORTANT — why the behavioral acceptance is DISABLED, not green:
//
// NET-2.6's deliverable is *pure-Cajeta control flow* (resolve, then
// try each address until one connects). That code is implemented and
// parses as stdlib. But its ONE observable branch point — whether an
// individual TcpStream.connect(#SocketAddress) attempt connects or
// throws — depends on the NET-1.3 socket-op *intrinsic codegen* (the
// `cajeta.net.TcpStream`-receiver dispatch block that should live in
// src/cajeta/asn/expression/MethodCallExpression.cpp, mirroring the
// `cajeta.io.file.File` dispatch there). That codegen is NOT yet
// implemented: TcpStream.connect/read/write/close are inert stubs
// (connect returns `heap TcpStream(-1)` unconditionally — it neither
// performs a real connect nor raises on a dead address). NetGetNameTests
// notes the same gap: "the full Cajeta-surface TcpStream
// connect/read/write/close round-trip ... is pinned by the JIT suite
// once that codegen lands."
//
// Until the TcpStream intrinsic dispatch lands, a fall-through test
// cannot distinguish a dead address from a live one (every stub
// connect "succeeds"), so a green here would be a false positive.
// These cases are therefore registered DISABLED_ to pin the NET-2.6
// acceptance precisely without claiming a verification the tree cannot
// yet provide. Drop the `DISABLED_` prefix once the NET-1.3 TcpStream
// socket-op codegen is wired and `connect(#SocketAddress)` truly
// connects/raises.
// =====================================================================

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

// Wrap a method body in a class importing the connect surface + the
// address types it needs. The body must `return` an int32.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.net.IpAddress;\n"
           "import cajeta.net.SocketAddress;\n"
           "import cajeta.net.TcpStream;\n"
           "import cajeta.net.NetException;\n"
           "import cajeta.net.ConnectionRefusedException;\n"
           "public final class A {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// ===================== fall back from a dead first address ================
//
// Plan acceptance (DnsTests.connectFallsThroughAddressList): "connect(host,
// port) falls back from a dead first address to a live second." A loopback
// listener is stood up by a sibling fiber (or the harness); resolving
// "localhost" can yield both ::1 and 127.0.0.1 — connect must skip the one
// that refuses and return a stream to the one that accepts. Returns 1 iff a
// connected stream comes back (fd >= 0).
//
// DISABLED until the NET-1.3 TcpStream socket-op intrinsic codegen lands
// (see file header) — the stub connect cannot model dead-vs-live.
TEST(NetConnectFallbackTests, DISABLED_connectFallsThroughAddressList) {
    EXPECT_EQ(runI32(makeSource(
        // A live loopback listener on `port` is assumed established by the
        // harness before run(); see the file header for why this is gated.
        "TcpStream s = TcpStream.connect(\"localhost\", 80);\n"
        "if (s.fd < 0) { return 0; }\n"
        "s.close();\n"
        "return 1;")), 1);
}

// ===================== all addresses dead → citing raise ==================
//
// When every resolved address refuses, connect(host, port) raises a
// ConnectionRefusedException whose message names the host (the actionable
// `connectRefusedCitesAddress` discipline applied at the host level).
//
// DISABLED for the same reason: the stub connect never refuses, so the
// loop never reaches the all-failed raise.
TEST(NetConnectFallbackTests, DISABLED_allAddressesDeadRaisesCitingHost) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        // Resolves, but port 1 on loopback has nothing listening, so every
        // candidate refuses and the loop exhausts.
        "    TcpStream s = TcpStream.connect(\"127.0.0.1\", 1);\n"
        "    return -1;\n"   // should not reach here
        "} catch (ConnectionRefusedException e) {\n"
        "    return (e.kind == NetException.KIND_CONNECTION_REFUSED) ? 1 : 0;\n"
        "}")), 1);
}

// ===================== a resolution failure propagates ====================
//
// A failed *resolution* (bad host) is a distinct fault from
// "resolved-but-unreachable": connect(host, port) lets the Dns.resolve
// NetException propagate unchanged rather than masking it as a connect
// refusal. This half exercises only the resolve leg (a working @Native
// bridge) plus the immediate propagation, so it does NOT depend on the
// TcpStream connect codegen — but it is kept DISABLED alongside its
// siblings so the whole NET-2.6 acceptance flips green together once the
// dependency lands and the suite can run end-to-end.
TEST(NetConnectFallbackTests, DISABLED_resolutionFailurePropagates) {
    EXPECT_EQ(runI32(makeSource(
        "try {\n"
        "    TcpStream s = TcpStream.connect(\"no.such.host.cajeta.invalid\", 80);\n"
        "    return -1;\n"   // should not reach here
        "} catch (NetException e) {\n"
        "    return 1;\n"
        "}")), 1);
}
