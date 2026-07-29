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
TEST(NetMulticastSurfaceTests, surfaceOptionRoundTrip) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress any = IpAddress.anyV4();\n"
        "SocketAddress bindAddr = SocketAddress.of(#any, 0);\n"
        "UdpSocket s = UdpSocket.bind(bindAddr);\n"
        // TTL: exact round-trip on the v4 u_char path.
        "s.setMulticastTtl(4);\n"
        "if (s.getMulticastTtl() != 4) { return 0; }\n"
        // Loopback: toggle off, read back, restore.
        "s.setMulticastLoopback(false);\n"
        "if (s.getMulticastLoopback()) { return 0; }\n"
        "s.setMulticastLoopback(true);\n"
        "if (!s.getMulticastLoopback()) { return 0; }\n"
        // Outbound v4 interface: pin to loopback, read it back.
        "IpAddress lo = IpAddress.loopbackV4();\n"
        "s.setMulticastInterface(lo);\n"
        "IpAddress got = s.getMulticastInterface();\n"
        "if (!got.isV4()) { return 0; }\n"
        // Join + leave a 239.x group via the loopback interface (v4 named
        // interface path); both must complete without throwing.
        "IpAddress group = IpAddress.fromV4(239, 255, 77, 89);\n"
        "IpAddress lo2 = IpAddress.loopbackV4();\n"
        "s.joinGroupOn(group, lo2);\n"
        "s.leaveGroupOn(group, lo2);\n"
        "s.close();\n"
        "return 1;")), 1);
}

// --- surface validation: family mismatch throws before any native call ------
TEST(NetMulticastSurfaceTests, familyMismatchThrows) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress any = IpAddress.anyV4();\n"
        "SocketAddress bindAddr = SocketAddress.of(#any, 0);\n"
        "UdpSocket s = UdpSocket.bind(bindAddr);\n"
        "IpAddress group = IpAddress.fromV4(239, 255, 77, 90);\n"
        "int32 caught = 0;\n"
        "try {\n"
        "    s.joinGroupOnIndex(group, 0);\n"     // v4 group down the v6 path
        "} catch (MalformedAddressException e) {\n"
        "    caught = 1;\n"
        "}\n"
        "s.close();\n"
        "return caught;")), 1);
}

// --- NET-14.3: group datagram round-trip over the Cajeta surface ------------
TEST(NetMulticastSurfaceTests, groupDatagramRoundTrip) {
#if defined(_WIN32)
    GTEST_SKIP() << "delivery semantics are validated on POSIX CI";
#else
    EXPECT_EQ(runI32(makeSource(
        // Receiver: bind ANY:0, join the group on loopback.
        "IpAddress any = IpAddress.anyV4();\n"
        "SocketAddress rxAddr = SocketAddress.of(#any, 0);\n"
        "UdpSocket rx = UdpSocket.bind(rxAddr);\n"
        "SocketAddress rxLocal = rx.localAddress();\n"
        "int32 port = rxLocal.getPort();\n"
        "if (port <= 0) { return 0; }\n"
        "IpAddress group = IpAddress.fromV4(239, 255, 77, 91);\n"
        "IpAddress lo = IpAddress.loopbackV4();\n"
        "rx.joinGroupOn(group, lo);\n"
        // Sender: pin the outbound interface to loopback, loop on.
        "IpAddress any2 = IpAddress.anyV4();\n"
        "SocketAddress txAddr = SocketAddress.of(#any2, 0);\n"
        "UdpSocket tx = UdpSocket.bind(txAddr);\n"
        "IpAddress lo2 = IpAddress.loopbackV4();\n"
        "tx.setMulticastInterface(lo2);\n"
        "tx.setMulticastLoopback(true);\n"
        // Send one datagram to group:port; receive it on the joined socket.
        "IpAddress gdst = IpAddress.fromV4(239, 255, 77, 91);\n"
        "SocketAddress dest = SocketAddress.of(#gdst, port);\n"
        "int8[] msg = heap int8[2];\n"
        "msg[0] = (int8) 71;\n"    // 'G'
        "msg[1] = (int8) 52;\n"    // '4'
        "if (tx.sendTo(msg, 0, 2, dest) != 2) { return 0; }\n"
        "int8[] buf = heap int8[16];\n"
        "RecvResult r = rx.recvFrom(buf, 0, 16);\n"
        "if (r.getCount() != 2) { return 0; }\n"
        "if (buf[0] != 71) { return 0; }\n"
        "if (buf[1] != 52) { return 0; }\n"
        "tx.close();\n"
        "rx.close();\n"
        "return 1;")), 1);
#endif
}

// --- NET-14.4: recvFromAsync parks and wakes on group traffic ---------------
TEST(NetMulticastSurfaceTests, recvFromAsyncWakesOnGroupTraffic) {
#if defined(_WIN32)
    GTEST_SKIP() << "delivery semantics are validated on POSIX CI";
#else
    EXPECT_EQ(runI32(makeAsyncSource(
        "IpAddress any = IpAddress.anyV4();\n"
        "SocketAddress rxAddr = SocketAddress.of(#any, 0);\n"
        "UdpSocket rx = UdpSocket.bind(rxAddr);\n"
        "SocketAddress rxLocal = rx.localAddress();\n"
        "int32 port = rxLocal.getPort();\n"
        "if (port <= 0) { return 0; }\n"
        "IpAddress group = IpAddress.fromV4(239, 255, 77, 92);\n"
        "IpAddress lo = IpAddress.loopbackV4();\n"
        "rx.joinGroupOn(group, lo);\n"
        "IpAddress any2 = IpAddress.anyV4();\n"
        "SocketAddress txAddr = SocketAddress.of(#any2, 0);\n"
        "UdpSocket tx = UdpSocket.bind(txAddr);\n"
        "IpAddress lo2 = IpAddress.loopbackV4();\n"
        "tx.setMulticastInterface(lo2);\n"
        "tx.setMulticastLoopback(true);\n"
        "IpAddress gdst = IpAddress.fromV4(239, 255, 77, 92);\n"
        "SocketAddress dest = SocketAddress.of(#gdst, port);\n"
        "int8[] msg = heap int8[1];\n"
        "msg[0] = (int8) 77;\n"    // 'M'
        "if (tx.sendTo(msg, 0, 1, dest) != 1) { return 0; }\n"
        // recvFromAsync parks the fiber on the reactor; the group datagram
        // (already queued, level-triggered) must satisfy it.
        "int8[] buf = heap int8[8];\n"
        "RecvResult r = rx.recvFromAsync(buf, 0, 8);\n"
        "if (r.getCount() != 1) { return 0; }\n"
        "if (buf[0] != 77) { return 0; }\n"
        "tx.close();\n"
        "rx.close();\n"
        "return 1;")), 1);
#endif
}
