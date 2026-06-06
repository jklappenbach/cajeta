// Acceptance test for the b2 socket receiver-lowering increment
// (plan/cajeta-net-plan.md §(b) "b2 — Socket options + UdpSocket"): the NEW
// compiler lowering of (1) the typed socket-option get/set pairs on a
// connected TcpStream and (2) the UdpSocket datagram surface, both exercised
// end-to-end through the JIT on loopback.
//
//   - tcpOptionRoundTrip: bind+connect+accept a loopback TcpStream, then
//     set/get NoDelay (exact) + TTL (exact) + a recv buffer size (the OS may
//     round a buffer request, so assert "grew / stayed positive", not equality
//     — per cajeta_net_socket_options.c's note that the getter is the source
//     of truth). Lowering exercised: TcpStream.setNoDelay/getNoDelay,
//     setTtl/getTtl, setRecvBufferSize/getRecvBufferSize.
//
//   - udpLoopbackDatagram: bind two UdpSockets on 127.0.0.1:0, read B's
//     kernel-assigned port via localAddress().getPort(), sendTo a datagram
//     A→B, recvFrom on B, byte-compare the payload. UDP needs no handshake so
//     single-threaded loopback works. Lowering exercised: UdpSocket.bind
//     (socket SOCK_DGRAM + bind), localAddress (getsockname+unpack),
//     sendTo (sockaddr_pack+sendto), recvFrom (recvfrom+unpack -> RecvResult),
//     RecvResult.getCount, close.
//
// Harness mirrors UriParseTests / NetLoopbackEchoTest: compile a small cajeta
// source through the JIT, call run() -> int32, assert EXPECT_EQ(runI32(...), 1).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.NetOptUdp");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.net.IpAddress;\n"
           "import cajeta.net.SocketAddress;\n"
           "import cajeta.net.TcpStream;\n"
           "import cajeta.net.TcpListener;\n"
           "import cajeta.net.UdpSocket;\n"
           "import cajeta.net.RecvResult;\n"
           "public final class NetOptUdp {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- TCP socket-option round-trip --------------------------------------
// NoDelay + TTL are exact; the recv buffer size is asserted positive (the OS
// may round/clamp/double the request, so byte-equality is not guaranteed).

TEST(NetOptionsUdpTest, tcpOptionRoundTrip) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress la = IpAddress.loopbackV4();\n"
        "SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "TcpListener listener = TcpListener.bind(bindAddr);\n"
        "int32 port = listener.boundPort();\n"
        "if (port <= 0) { return 0; }\n"
        "IpAddress ca = IpAddress.loopbackV4();\n"
        "SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "TcpStream client = TcpStream.connect(#connAddr);\n"
        "TcpStream server = listener.accept();\n"
        // NoDelay: exact round-trip (false -> true).
        "client.setNoDelay(true);\n"
        "boolean nd = client.getNoDelay();\n"
        // TTL: exact round-trip.
        "client.setTtl(7);\n"
        "int32 ttl = client.getTtl();\n"
        // Recv buffer: OS may round; assert it read back positive.
        "client.setRecvBufferSize(65536);\n"
        "int32 rbuf = client.getRecvBufferSize();\n"
        "client.close();\n"
        "server.close();\n"
        "listener.close();\n"
        "if (!nd) { return 0; }\n"
        "if (ttl != 7) { return 0; }\n"
        "if (rbuf <= 0) { return 0; }\n"
        "return 1;")), 1);
}

// --- UDP loopback datagram ---------------------------------------------
// A→B single-datagram round-trip, byte-comparing the payload.

TEST(NetOptionsUdpTest, udpLoopbackDatagram) {
    EXPECT_EQ(runI32(makeSource(
        // Bind B on an ephemeral loopback port.
        "IpAddress bip = IpAddress.loopbackV4();\n"
        "SocketAddress bAddr = SocketAddress.of(#bip, 0);\n"
        "UdpSocket b = UdpSocket.bind(bAddr);\n"
        "SocketAddress bLocal = b.localAddress();\n"
        "int32 bPort = bLocal.getPort();\n"
        "if (bPort <= 0) { return 0; }\n"
        // Bind A (any local ephemeral port).
        "IpAddress aip = IpAddress.loopbackV4();\n"
        "SocketAddress aAddr = SocketAddress.of(#aip, 0);\n"
        "UdpSocket a = UdpSocket.bind(aAddr);\n"
        // Datagram A -> B's bound port.
        "IpAddress dip = IpAddress.loopbackV4();\n"
        "SocketAddress dest = SocketAddress.of(#dip, bPort);\n"
        "int8[] payload = heap int8[4];\n"
        "payload[0] = (int8) 100;\n"   // 'd'
        "payload[1] = (int8) 103;\n"   // 'g'
        "payload[2] = (int8) 114;\n"   // 'r'
        "payload[3] = (int8) 109;\n"   // 'm'
        "int32 sent = a.sendTo(payload, 0, 4, dest);\n"
        "if (sent != 4) { return 0; }\n"
        // Receive on B.
        "int8[] rbuf = heap int8[8];\n"
        "RecvResult rr = b.recvFrom(rbuf, 0, 8);\n"
        "int32 got = rr.getCount();\n"
        "a.close();\n"
        "b.close();\n"
        "if (got != 4) { return 0; }\n"
        "int32 i = 0;\n"
        "while (i < 4) {\n"
        "    if (rbuf[i] != payload[i]) { return 0; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return 1;")), 1);
}
