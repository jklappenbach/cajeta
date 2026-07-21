//
// NET-3.3 (UDP half) — UdpSocket.recvFromAsync: the fiber-parking datagram
// receive. Await-first readiness over the level-triggered reactor: a
// datagram already queued means awaitReadable returns immediately and the
// non-blocking recvFrom collects it (mirrors NetAsyncEchoTest's
// acceptAsyncReturnsPendingConnection shape — single fiber, send first).
// The park/wake path itself is pinned by the reactor's own tests; this
// pins the UdpSocket surface + result plumbing.
//
// First consumer: dev.cajeta.gossip's receive loop.
//

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

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.IpAddress;\n"
           "import cajeta.io.net.SocketAddress;\n"
           "import cajeta.io.net.UdpSocket;\n"
           "import cajeta.io.net.RecvResult;\n"
           "import cajeta.concurrent.Tasks;\n"
           "public final class U {\n"
           "    public static int32 run() {\n"
           "        () -> int32 body = () -> {\n"
           "        " + body + "\n"
           "        };\n"
           "        return Tasks.runBlocking<int32>(body);\n"
           "    }\n"
           "}\n";
}

} // namespace

TEST(NetUdpAsyncTest, recvFromAsyncCollectsPendingDatagram) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress la = IpAddress.loopbackV4();\n"
        "SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "UdpSocket rx = UdpSocket.bind(bindAddr);\n"
        "SocketAddress rxAddr = rx.localAddress();\n"
        "IpAddress lb = IpAddress.loopbackV4();\n"
        "SocketAddress txBind = SocketAddress.of(#lb, 0);\n"
        "UdpSocket tx = UdpSocket.bind(txBind);\n"
        "int8[] msg = heap int8[3];\n"
        "msg[0] = (int8) 103;\n"   // 'g'
        "msg[1] = (int8) 115;\n"   // 's'
        "msg[2] = (int8) 112;\n"   // 'p'
        "int32 sent = tx.sendTo(msg, 0, 3, rxAddr);\n"
        "if (sent != 3) { return 0; }\n"
        "int8[] buf = heap int8[64];\n"
        "RecvResult r = rx.recvFromAsync(buf, 0, 64);\n"
        "if (r.getCount() != 3) { return 0; }\n"
        "if (buf[0] != 103) { return 0; }\n"
        "if (buf[2] != 112) { return 0; }\n"
        "tx.close();\n"
        "rx.close();\n"
        "return 1;")), 1);
}

// Two datagrams: the loop re-arms — second recvFromAsync collects the
// second datagram (readiness is level-triggered, not consumed by the
// first receive).
TEST(NetUdpAsyncTest, recvFromAsyncReArms) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress la = IpAddress.loopbackV4();\n"
        "SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "UdpSocket rx = UdpSocket.bind(bindAddr);\n"
        "SocketAddress rxAddr = rx.localAddress();\n"
        "IpAddress lb = IpAddress.loopbackV4();\n"
        "SocketAddress txBind = SocketAddress.of(#lb, 0);\n"
        "UdpSocket tx = UdpSocket.bind(txBind);\n"
        "int8[] m1 = heap int8[1];\n"
        "m1[0] = (int8) 1;\n"
        "int8[] m2 = heap int8[1];\n"
        "m2[0] = (int8) 2;\n"
        "tx.sendTo(m1, 0, 1, rxAddr);\n"
        "tx.sendTo(m2, 0, 1, rxAddr);\n"
        "int8[] buf = heap int8[16];\n"
        "RecvResult r1 = rx.recvFromAsync(buf, 0, 16);\n"
        "if (r1.getCount() != 1) { return 0; }\n"
        "if (buf[0] != 1) { return 0; }\n"
        "RecvResult r2 = rx.recvFromAsync(buf, 0, 16);\n"
        "if (r2.getCount() != 1) { return 0; }\n"
        "if (buf[0] != 2) { return 0; }\n"
        "tx.close();\n"
        "rx.close();\n"
        "return 1;")), 1);
}
