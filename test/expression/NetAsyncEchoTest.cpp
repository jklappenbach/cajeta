// Acceptance test for the b3 async-socket increment
// (plan/cajeta-net-plan.md §(b) "b3 — Async TCP surface (reactor + readiness
// loops)"): a Cajeta-surface TCP loopback echo that round-trips end-to-end
// through the JIT **using the async ops** (`readAsync` / `writeAsync` /
// `writeAllAsync` / `acceptAsync`) driven from inside a real fiber. Exercises
// the b3 surface: TcpStream.readAsync/writeAsync/writeAllAsync,
// TcpListener.acceptAsync, the WouldBlock readiness loop, the @Native reactor
// bridges (cajeta.io.net.reactor.Reactor), and AsyncReader/AsyncWriter.
//
// The async ops put the fd into non-blocking mode and park the fiber on the
// reactor when a recv/send/accept reports WouldBlock. On Linux the park is the
// real epoll fiber-park (carrier stays free); on the Windows test host the
// await path is the portable select-based readiness probe (correct, but blocks
// the carrier — the honest v1 bring-up). Either way the readiness loop retries
// and the echo completes.
//
// Loopback discipline (single fiber, mirrors NetLoopbackEchoTest): the kernel
// completes the loopback 3-way handshake into the listen backlog when connect
// is issued, so a sequential connect → accept → write → read on one fiber is
// sound (no second thread needed). The async path still drives the full
// non-blocking + reactor machinery — a WouldBlock on the first recv (before the
// peer has sent) parks then resumes.
//
// Harness mirrors NetLoopbackEchoTest / RunBlockingTests: compile a small
// cajeta source through the JIT, call run() -> int32, assert it returns 1.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.NetAsyncEcho");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.IpAddress;\n"
           "import cajeta.io.net.SocketAddress;\n"
           "import cajeta.io.net.TcpStream;\n"
           "import cajeta.io.net.TcpListener;\n"
           "import cajeta.io.net.AsyncReader;\n"
           "import cajeta.io.net.AsyncWriter;\n"
           "import cajeta.concurrent.Tasks;\n"
           "public final class NetAsyncEcho {\n"
           "    public static int32 run() {\n"
           "        () -> int32 body = () -> {\n"
           "        " + body + "\n"
           "        };\n"
           "        return Tasks.runBlocking<int32>(body);\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- Full async loopback echo round-trip through a fiber ----------------
// → the b3 acceptance criterion: a Cajeta-surface async loopback echo passes,
//   driving the readiness loop + reactor park/resume.

TEST(NetAsyncEchoTest, asyncLoopbackEchoRoundTrips) {
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
        // client → server: async write "ping"
        "int8[] ping = heap int8[4];\n"
        "ping[0] = (int8) 112;\n"   // 'p'
        "ping[1] = (int8) 105;\n"   // 'i'
        "ping[2] = (int8) 110;\n"   // 'n'
        "ping[3] = (int8) 103;\n"   // 'g'
        "client.writeAsync(ping, (int64) 0, (int64) 4);\n"
        // server: async read the 4 bytes, then async echo them back
        "int8[] rbuf = heap int8[4];\n"
        "int64 got = server.readAsync(rbuf, (int64) 0, (int64) 4);\n"
        "if (got != 4) { return 0; }\n"
        "server.writeAllAsync(rbuf, (int64) 0, got);\n"
        // client: async read the echo back
        "int8[] echo = heap int8[4];\n"
        "int64 echoGot = client.readAsync(echo, (int64) 0, (int64) 4);\n"
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

// --- acceptAsync parks then resumes when a client connects --------------
// Single fiber: connect first (kernel completes the loopback handshake into the
// backlog), then acceptAsync returns the pending connection without parking; a
// readAsync of the client's bytes confirms the accepted stream is live.

TEST(NetAsyncEchoTest, acceptAsyncReturnsPendingConnection) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress la = IpAddress.loopbackV4();\n"
        "SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "TcpListener listener = TcpListener.bind(bindAddr);\n"
        "int32 port = listener.boundPort();\n"
        "if (port <= 0) { return 0; }\n"
        "IpAddress ca = IpAddress.loopbackV4();\n"
        "SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "TcpStream client = TcpStream.connect(#connAddr);\n"
        "TcpStream server = listener.acceptAsync();\n"
        "int8[] hi = heap int8[2];\n"
        "hi[0] = (int8) 72;\n"   // 'H'
        "hi[1] = (int8) 105;\n"  // 'i'
        "client.writeAsync(hi, (int64) 0, (int64) 2);\n"
        "int8[] rbuf = heap int8[2];\n"
        "int64 got = server.readAsync(rbuf, (int64) 0, (int64) 2);\n"
        "client.close();\n"
        "server.close();\n"
        "listener.close();\n"
        "if (got != 2) { return 0; }\n"
        "if (rbuf[0] != hi[0]) { return 0; }\n"
        "if (rbuf[1] != hi[1]) { return 0; }\n"
        "return 1;")), 1);
}

// --- AsyncReader / AsyncWriter buffered layer over the async stream -----
// Drive the buffered NET-3.5 layer end-to-end: an AsyncWriter coalesces a write
// and flushes it async; an AsyncReader buffers the async reads and serves a
// readExact. Proves the buffered seam composes over the b3 readiness loops.
//
// This had REGRESSED at commit 887ca39: it was green at b3 (eb8651f) when
// AsyncReader/AsyncWriter held a CONCRETE `TcpStream stream` field. The b7-b9
// ByteChannel keystone repointed that field to the `ByteChannel` INTERFACE so
// the same buffered layer could run over TLS — but `ByteChannel` is forward-
// referenced relative to AsyncReader, so the field had been laid out as a thin
// class pointer instead of a fat interface pointer and the interface-dispatch
// call `this.stream.readAsync(...)` was silently dropped at codegen (Bug 2).
// That compiler bug is now FIXED: a prescan marks interface declarations
// (g_interfaceArchive) so CajetaType::fromContext synthesizes a forward-
// referenced interface placeholder that is born FAT (24-byte {ptr,ptr,i64})
// and isInterface()=true, and visitInterfaceDeclaration REUSES that placeholder
// (fillFromDeclaration) so its method set lands on the same object every field
// captured — see memory `cajeta-interface-arg-field-offset-bug`. This row
// (the direct repro) is green again.
TEST(NetAsyncEchoTest, bufferedReaderWriterRoundTrips) {
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
        "AsyncWriter w = heap AsyncWriter(client);\n"
        "int8[] msg = heap int8[3];\n"
        "msg[0] = (int8) 97;\n"   // 'a'
        "msg[1] = (int8) 98;\n"   // 'b'
        "msg[2] = (int8) 99;\n"   // 'c'
        "w.writeAll(msg, 0, 3);\n"
        "w.flush();\n"
        "AsyncReader r = heap AsyncReader(server);\n"
        "int8[] dst = heap int8[3];\n"
        "r.readExact(dst, 0, 3);\n"
        "client.close();\n"
        "server.close();\n"
        "listener.close();\n"
        "int32 i = 0;\n"
        "while (i < 3) {\n"
        "    if (dst[i] != msg[i]) { return 0; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return 1;")), 1);
}

// --- connectAsync: non-blocking connect static lowering (NET-3.3) -------
// connectAsync makes the socket non-blocking, issues the connect, and — when
// it doesn't complete synchronously — parks the fiber on reactor writability
// then reads SO_ERROR. On loopback the connect completes immediately (the
// kernel finishes the handshake into the listen backlog), so this primarily
// exercises the immediate-success + wrap path of the new lowering; the await
// + SO_ERROR blocks are verified well-formed at module-verify time. The
// returned stream is a live, connected TcpStream: a full async echo round-trip
// over it confirms the fd was wrapped correctly.
TEST(NetAsyncEchoTest, connectAsyncLoopbackEchoRoundTrips) {
    EXPECT_EQ(runI32(makeSource(
        "IpAddress la = IpAddress.loopbackV4();\n"
        "SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "TcpListener listener = TcpListener.bind(bindAddr);\n"
        "int32 port = listener.boundPort();\n"
        "if (port <= 0) { return 0; }\n"
        "IpAddress ca = IpAddress.loopbackV4();\n"
        "SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "TcpStream client = TcpStream.connectAsync(#connAddr);\n"
        "TcpStream server = listener.accept();\n"
        "int8[] ping = heap int8[4];\n"
        "ping[0] = (int8) 112;\n"   // 'p'
        "ping[1] = (int8) 105;\n"   // 'i'
        "ping[2] = (int8) 110;\n"   // 'n'
        "ping[3] = (int8) 103;\n"   // 'g'
        "client.writeAsync(ping, (int64) 0, (int64) 4);\n"
        "int8[] rbuf = heap int8[4];\n"
        "int64 got = server.readAsync(rbuf, (int64) 0, (int64) 4);\n"
        "if (got != 4) { return 0; }\n"
        "server.writeAllAsync(rbuf, (int64) 0, got);\n"
        "int8[] echo = heap int8[4];\n"
        "int64 echoGot = client.readAsync(echo, (int64) 0, (int64) 4);\n"
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
