// NET-8.1 — the HTTP/1.1 client over a loopback socket.
//
// STATUS: `httpGetReturnsBody` (plaintext GET end-to-end) is GREEN. It was
// previously DISABLED on a "hang" that the prior session mis-attributed to a
// fragile live-socket codegen path; the real cause was a STDLIB defect in
// HttpClient.connectAny: it rebuilt the resolved address with
// `IpAddress.fromOctets(oct, oct.count())`, but `getOctets()` returns the raw
// 16-byte backing buffer regardless of family, so a V4 address (127.0.0.1) was
// re-interpreted as an IPv6 address and `connectAsync` parked forever on a
// bogus endpoint. Fixed by deriving the octet length from the address family
// (mirroring the DnsCache idiom). This row now exercises the resolve→connect→
// serialize→parse path live and is a regression guard for that fix.
//
// The HTTPS-server (HttpsServerTests) and WebSocket (WsEntryPointTests) live
// rows remain DISABLED on a SEPARATE, still-open compiler bug: a forward-
// referenced interface (e.g. `ByteChannel` used by AsyncReader/AsyncWriter
// before its declaration is visited) is laid out as a THIN class pointer
// instead of a fat interface pointer, so interface-dispatch calls through that
// field (`this.stream.readAsync(...)`) are silently dropped at codegen — the
// buffered reader/writer reads/writes garbage over a live socket. Root-caused
// in detail in memory `cajeta-interface-arg-field-offset-bug`; the complete fix
// requires reconciling forward-referenced interface placeholders to the
// canonical interface across field/param/local resolution (a structural type-
// resolution change). The protocol logic itself is covered by the pure-byte
// golden tests (HttpParser/HttpSerializer/BodyReader/KeepAlive suites).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Transport guard: a spawned server fiber acceptAsync's and echoes; the main
// fiber connectAsync's and round-trips. Pins the spawn + fiber-parking socket
// pattern the client connect path relies on.
TEST(HttpClientTests, transportSpawnAcceptConnectEcho) {
    std::string src =
        "package test;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.TcpListener;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class M {\n"
        "    public static async int32 srv(#TcpListener listener) {\n"
        "        TcpStream sock = listener.acceptAsync();\n"
        "        int8[] b = new int8[4];\n"
        "        int64 n = sock.readAsync(b, (int64) 0, (int64) 4);\n"
        "        sock.writeAllAsync(b, (int64) 0, n);\n"
        "        sock.close();\n"
        "        return (int32) n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TcpListener listener = TcpListener.bind(bindAddr);\n"
        "            int32 port = listener.boundPort();\n"
        "            if (port <= 0) { return -1; }\n"
        "            Task<int32> t = spawn srv(#listener);\n"
        "            IpAddress ca = IpAddress.loopbackV4();\n"
        "            SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "            TcpStream client = TcpStream.connectAsync(#connAddr);\n"
        "            int8[] ping = new int8[4];\n"
        "            ping[0L]=(int8)112; ping[1L]=(int8)105; ping[2L]=(int8)110; ping[3L]=(int8)103;\n"
        "            client.writeAllAsync(ping, (int64) 0, (int64) 4);\n"
        "            int8[] echo = new int8[4];\n"
        "            int64 g = client.readAsync(echo, (int64) 0, (int64) 4);\n"
        "            int32 sr = await t;\n"
        "            client.close();\n"
        "            if (sr != 4) { return -2; }\n"
        "            if (g != (int64) 4) { return -3; }\n"
        "            if (echo[0L] != (int8) 112) { return -4; }\n"
        "            return 1;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Transport guard: a TcpStream upcast to a ByteChannel, calling
// readAsync/writeAllAsync through the interface — the plaintext-vs-TLS seam the
// client/server read and write through (NET-8.1 / NET-9.5).
TEST(HttpClientTests, byteChannelInterfaceDispatchEcho) {
    std::string src =
        "package test;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.TcpListener;\n"
        "import cajeta.net.ByteChannel;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class M {\n"
        "    public static async int32 srv(#TcpListener listener) {\n"
        "        TcpStream sock = listener.acceptAsync();\n"
        "        ByteChannel sch = sock;\n"
        "        int8[] b = new int8[4];\n"
        "        int64 n = sch.readAsync(b, (int64) 0, (int64) 4);\n"
        "        sch.writeAllAsync(b, (int64) 0, n);\n"
        "        sock.close();\n"
        "        return (int32) n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TcpListener listener = TcpListener.bind(bindAddr);\n"
        "            int32 port = listener.boundPort();\n"
        "            if (port <= 0) { return -1; }\n"
        "            Task<int32> t = spawn srv(#listener);\n"
        "            IpAddress ca = IpAddress.loopbackV4();\n"
        "            SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "            TcpStream sock = TcpStream.connectAsync(#connAddr);\n"
        "            ByteChannel ch = sock;\n"
        "            int8[] ping = new int8[4];\n"
        "            ping[0L]=(int8)112; ping[1L]=(int8)105; ping[2L]=(int8)110; ping[3L]=(int8)103;\n"
        "            ch.writeAllAsync(ping, (int64) 0, (int64) 4);\n"
        "            int8[] echo = new int8[4];\n"
        "            int64 g = ch.readAsync(echo, (int64) 0, (int64) 4);\n"
        "            int32 sr = await t;\n"
        "            sock.close();\n"
        "            if (sr != 4) { return -2; }\n"
        "            if (g != (int64) 4) { return -3; }\n"
        "            if (echo[0L] != (int8) 112) { return -4; }\n"
        "            return 1;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Pure (no socket): AsyncReader.stage() then read() drains the staging ring —
// the buffered-reader logic the server loop uses, validated without I/O.
TEST(HttpClientTests, asyncReaderStageDrainPure) {
    std::string src =
        "package test;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.AsyncReader;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        TcpStream t = heap TcpStream(-1);\n"
        "        AsyncReader r = heap AsyncReader(t);\n"
        "        int8[] s = new int8[4];\n"
        "        s[0L]=(int8)112; s[1L]=(int8)105; s[2L]=(int8)110; s[3L]=(int8)103;\n"
        "        int32 w = r.stage(s, 4);\n"
        "        int8[] dst = new int8[4];\n"
        "        int32 g = r.read(dst, 0, 4);\n"
        "        if (w != 4) { return -1; }\n"
        "        if (g != 4) { return -2; }\n"
        "        return (int32) dst[0L];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 112);
}

namespace {
const std::string kResp =
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
std::string emitBytes(const std::string& name, const std::string& data) {
    std::string s = "        int8[] " + name + " = new int8["
                  + std::to_string(data.size()) + "];\n";
    for (size_t i = 0; i < data.size(); i++) {
        s += "        " + name + "[" + std::to_string(i) + "L] = (int8) "
           + std::to_string((int) (signed char) data[i]) + ";\n";
    }
    return s;
}
} // namespace

// ACCEPTANCE: a plaintext GET to a loopback server fiber returns 200 + "hello".
// Exercises resolve (Dns) -> connect (connectAny) -> serialize -> parse live.
// Regression guard for the connectAny IPv4-octet-length fix (see file header).
TEST(HttpClientTests, httpGetReturnsBody) {
    std::string src =
        "package test;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.TcpListener;\n"
        "import cajeta.net.uri.Uri;\n"
        "import cajeta.net.http.HttpClient;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class M {\n"
        "    public static async int32 plainServer(#TcpListener listener, int8[] resp, int32 respLen) {\n"
        "        TcpStream sock = listener.acceptAsync();\n"
        "        int8[] inbuf = new int8[2048];\n"
        "        int64 n = sock.readAsync(inbuf, (int64) 0, (int64) 2048);\n"
        "        sock.writeAllAsync(resp, (int64) 0, (int64) respLen);\n"
        "        sock.close();\n"
        "        return (int32) n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        + emitBytes("resp", kResp) +
        "            int32 rl = " + std::to_string(kResp.size()) + ";\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TcpListener listener = TcpListener.bind(bindAddr);\n"
        "            int32 port = listener.boundPort();\n"
        "            if (port <= 0) { return -1; }\n"
        "            Task<int32> serverTask = spawn plainServer(#listener, resp, rl);\n"
        "            HttpClient client = heap HttpClient();\n"
        "            Uri uri = Uri.builder().scheme(\"http\").host(\"127.0.0.1\").port(port).path(\"/\").build();\n"
        "            HttpRequest req = HttpRequest.fromUri(\"GET\", uri);\n"
        "            HttpResponse resp2 = client.send(uri, req);\n"
        "            int32 sret = await serverTask;\n"
        "            if (resp2.statusCode() != 200) { return -2; }\n"
        "            if (resp2.bodyLength() != 5) { return -3; }\n"
        "            int8[] rb = resp2.body;\n"
        "            if (rb[0L] != (int8) 104) { return -4; }\n"
        "            return 1;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
