// Acceptance test for the b1 socket receiver-lowering increment
// (plan/cajeta-net-plan.md §(b) "b1 — Address stack + TcpStream sync I/O +
// loopback echo"): a Cajeta-surface TCP loopback echo round-trips end-to-end
// through the JIT, exercising the NEW compiler lowering of the socket surface
// to the `__cajeta_net_*` intrinsics.
//
// The single Cajeta `run()` below:
//   1. binds a TcpListener on 127.0.0.1:0 (ephemeral port),
//   2. reads back the kernel-assigned port via boundPort(),
//   3. TcpStream.connect()s a client to it (blocking; on loopback the kernel
//      completes the handshake into the listen backlog so connect returns
//      without a concurrent accept — single-threaded is fine),
//   4. accept()s the connection server-side,
//   5. writes "ping" client→server, the server reads it and echoes it back,
//   6. the client reads the echo and compares it byte-for-byte,
//   returning 1 on a perfect round-trip / 0 otherwise.
//
// Lowering exercised: TcpListener.bind (socket+reuseaddr+bind+listen),
// TcpListener.boundPort (getsockname+unpack), TcpListener.accept/acceptFd
// (accept), TcpStream.connect (sockaddr_pack+socket+connect), TcpStream.write
// (send), TcpStream.read (recv), close (close).
//
// Harness mirrors UriParseTests: compile a small cajeta source through the
// JIT, call run() -> int32, assert EXPECT_EQ(runI32(...), 1).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.NetEcho");
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
           "public final class NetEcho {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- Full loopback echo round-trip -------------------------------------
// → the b1 acceptance criterion (Cajeta-surface loopback echo passes).

TEST(NetLoopbackEchoTest, loopbackEchoRoundTrips) {
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
        "int8[] ping = heap int8[4];\n"
        "ping[0] = (int8) 112;\n"   // 'p'
        "ping[1] = (int8) 105;\n"   // 'i'
        "ping[2] = (int8) 110;\n"   // 'n'
        "ping[3] = (int8) 103;\n"   // 'g'
        "client.write(ping, (int64) 0, (int64) 4);\n"
        "int8[] rbuf = heap int8[4];\n"
        "int64 got = server.read(rbuf, (int64) 0, (int64) 4);\n"
        "server.write(rbuf, (int64) 0, got);\n"
        "int8[] echo = heap int8[4];\n"
        "int64 echoGot = client.read(echo, (int64) 0, (int64) 4);\n"
        "client.close();\n"
        "server.close();\n"
        "listener.close();\n"
        "if (echoGot != 4) { return 0; }\n"
        "int32 i = 0;\n"
        "while (i < 4) {\n"
        "    if (echo[i] != ping[i]) { return 0; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return 1;")), 1);
}

// --- boundPort resolves an ephemeral port ------------------------------

TEST(NetLoopbackEchoTest, ephemeralBindReportsNonZeroPort) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress la = IpAddress.loopbackV4();\n"
        "SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "TcpListener listener = TcpListener.bind(bindAddr);\n"
        "int32 port = listener.boundPort();\n"
        "listener.close();\n"
        "return port > 0 ? 1 : 0;")), 1);
}
