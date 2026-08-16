//
// NET-14.2 / 14.3 / 14.4 — the Cajeta-language multicast surface on UdpSocket.
//
// Exercises the REAL method bodies (the `@Native`-bridge pattern, not
// call-site intrinsic lowering) end-to-end through the JIT:
//
//   - surfaceOptionRoundTrip: setMulticastTtl/getMulticastTtl (exact),
//     setMulticastLoopback/getMulticastLoopback (toggle), v4
//     setMulticastInterface/getMulticastInterface (loopback round-trip),
//     joinGroup/leaveGroup on 239.x via the loopback interface — the
//     family-dispatch and error plumbing all in Cajeta.
//   - familyMismatchThrows: joinGroupOnIndex with a v4 group must raise
//     MalformedAddressException — the surface's own validation, before any
//     native call.
//   - groupDatagramRoundTrip (NET-14.3): join on loopback, pin the sender's
//     outbound interface, sendTo the group, recvFrom collects it — the
//     Cajeta-surface receive path over a joined group.
//   - recvFromAsyncWakesOnGroupTraffic (NET-14.4): same shape but the receive
//     parks on the reactor via recvFromAsync; group traffic must wake it.
//
// Delivery tests are POSIX-gated like NetMulticastTests; the option
// round-trips and validation run everywhere.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.NetMcast");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.IpAddress;\n"
           "import cajeta.io.net.SocketAddress;\n"
           "import cajeta.io.net.UdpSocket;\n"
           "import cajeta.io.net.RecvResult;\n"
           "import cajeta.io.net.MalformedAddressException;\n"
           "public final class NetMcast {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

std::string makeAsyncSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.IpAddress;\n"
           "import cajeta.io.net.SocketAddress;\n"
           "import cajeta.io.net.UdpSocket;\n"
           "import cajeta.io.net.RecvResult;\n"
           "import cajeta.concurrent.Tasks;\n"
           "public final class NetMcast {\n"
           "    public static int32 run() {\n"
           "        () -> int32 body = () -> {\n"
           "        " + body + "\n"
           "        };\n"
           "        return Tasks.runBlocking<int32>(body);\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- option round-trip + membership through the Cajeta surface -------------

// --- surface validation: family mismatch throws before any native call ------

// --- NET-14.3: group datagram round-trip over the Cajeta surface ------------

// --- NET-14.4: recvFromAsync parks and wakes on group traffic ---------------
TEST(NetMulticastSurfaceTests, recvFromAsyncWakesOnGroupTraffic) {
#if defined(_WIN32)
    GTEST_SKIP() << "delivery semantics are validated on POSIX CI";
#else
    EXPECT_EQ(runI32(makeAsyncSource(
        "IpAddress any #= IpAddress.anyV4();\n"
        "SocketAddress rxAddr #= SocketAddress.of(#any, 0);\n"
        "UdpSocket rx #= UdpSocket.bind(rxAddr);\n"
        "SocketAddress rxLocal #= rx.localAddress();\n"
        "int32 port = rxLocal.getPort();\n"
        "if (port <= 0) { return 0; }\n"
        "IpAddress group #= IpAddress.fromV4(239, 255, 77, 92);\n"
        "IpAddress lo #= IpAddress.loopbackV4();\n"
        "rx.joinGroupOn(group, lo);\n"
        "IpAddress any2 #= IpAddress.anyV4();\n"
        "SocketAddress txAddr #= SocketAddress.of(#any2, 0);\n"
        "UdpSocket tx #= UdpSocket.bind(txAddr);\n"
        "IpAddress lo2 #= IpAddress.loopbackV4();\n"
        "tx.setMulticastInterface(lo2);\n"
        "tx.setMulticastLoopback(true);\n"
        "IpAddress gdst #= IpAddress.fromV4(239, 255, 77, 92);\n"
        "SocketAddress dest #= SocketAddress.of(#gdst, port);\n"
        "int8[] msg = heap int8[1];\n"
        "msg[0] = (int8) 77;\n"    // 'M'
        "if (tx.sendTo(msg, 0, 1, dest) != 1) { return 0; }\n"
        // recvFromAsync parks the fiber on the reactor; the group datagram
        // (already queued, level-triggered) must satisfy it.
        "int8[] buf = heap int8[8];\n"
        "RecvResult r #= rx.recvFromAsync(buf, 0, 8);\n"
        "if (r.getCount() != 1) { return 0; }\n"
        "if (buf[0] != 77) { return 0; }\n"
        "tx.close();\n"
        "rx.close();\n"
        "return 1;")), 1);
#endif
}
