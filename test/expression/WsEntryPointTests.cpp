// NET-10.1 / NET-10.7 -- the WebSocket client + server entry points.
//
// The PURE handshake interop (clientHandshakeRoundTripsWithServer) runs the
// NET-10.1 client opening handshake (WsClientHandshake) against the NET-10.2
// server handshake (WsServerHandshake) with no socket -- proving the request the
// client builds is accepted and that the client validates the server's
// Sec-WebSocket-Accept.
//
// The full live round-trip drives a real loopback socket through WsUpgrade ->
// WebSocket via AsyncReader/AsyncWriter, echoing one binary message. It was
// long disabled chasing a "0-length message" use-after-free that was wrongly
// blamed first on the forward-referenced-interface codegen bug (Bug 2) and then
// on a "fiber park x scope-exit drop" compiler defect. Both were red herrings.
//
// ROOT CAUSE (fixed): WsFrameDecoder double-freed every decoded frame.
// finishIfPayloadComplete built `WsFrame f = WsFrame.of(...)` then called
// `enqueue(f)` WITHOUT a `#` transfer, and nextFrame() returned a non-`#`
// WsFrame. So the local `f` stayed an active owner and its scope-exit drop
// reclaimed the frame the queue still referenced; nextFrame() then handed the
// read loop a freed frame -> use-after-free on the frame's payload. It passed
// in isolation (freed memory still readable) and failed under heap churn -- the
// fiber park only ADDED churn, it was never the cause. Fix: WsFrameDecoder now
// `#`-transfers frames into the queue (`enqueue(#f)` / `queue[t] = #f`) and out
// of nextFrame (`#WsFrame` / `return #f`), so the frame has exactly one owner.
// Verified deterministically with --poison-free (which turns the UAF into a
// hard crash): both the pure decoder path and this live echo are green under
// poison. The same bug also broke WsFrameCodecTests.decodeGoldenVectors.

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

// PURE (no socket): the client handshake (NET-10.1) interoperates with the
// server handshake (NET-10.2). Client builds the Upgrade request; server
// validates + answers 101; client validates the Sec-WebSocket-Accept.
TEST(WsEntryPointTests, clientHandshakeRoundTripsWithServer) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.uri.Uri;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.net.ws.WsClientHandshake;\n"
        "import cajeta.net.ws.WsServerHandshake;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        Uri uri = Uri.builder().scheme(\"ws\").host(\"example.test\").port(80).path(\"/chat\").build();\n"
        "        String key = WsClientHandshake.placeholderKey(42);\n"
        "        HttpRequest req = WsClientHandshake.buildRequest(uri, key);\n"
        "        if (!WsServerHandshake.isUpgradeRequest(req)) { return -1; }\n"
        "        HttpResponse resp = WsServerHandshake.accept(req);\n"
        "        if (resp.statusCode() != 101) { return -2; }\n"
        "        if (!WsClientHandshake.validateAccept(resp, key)) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// PURE (no socket): distinct seeds yield distinct keys, and a key validates only
// against its own computed accept token (a wrong key is rejected).
TEST(WsEntryPointTests, handshakeKeyBindingIsChecked) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.uri.Uri;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.net.ws.WsClientHandshake;\n"
        "import cajeta.net.ws.WsServerHandshake;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        String k1 = WsClientHandshake.placeholderKey(1);\n"
        "        String k2 = WsClientHandshake.placeholderKey(2);\n"
        "        if (k1.equals(k2)) { return -1; }\n"            // distinct seeds -> distinct keys
        "        Uri uri = Uri.builder().scheme(\"ws\").host(\"h\").port(80).path(\"/\").build();\n"
        "        HttpRequest req = WsClientHandshake.buildRequest(uri, k1);\n"
        "        HttpResponse resp = WsServerHandshake.accept(req);\n"
        "        if (WsClientHandshake.validateAccept(resp, k2)) { return -2; }\n"  // wrong key rejected
        "        if (!WsClientHandshake.validateAccept(resp, k1)) { return -3; }\n" // right key accepted
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// FULL LIVE ROUND-TRIP: client + server WebSockets over a loopback socket,
// echoing one binary message. Re-enabled after fixing the WsFrameDecoder
// double-free (see file header) -- the bytes round-trip and the reassembled
// message keeps its 4-byte payload through the read loop.
TEST(WsEntryPointTests, clientServerEchoRoundTripOverLoopback) {
    std::string src =
        "package test;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.TcpListener;\n"
        "import cajeta.net.AsyncReader;\n"
        "import cajeta.net.AsyncWriter;\n"
        "import cajeta.net.uri.Uri;\n"
        "import cajeta.net.ws.WebSocket;\n"
        "import cajeta.net.ws.WsMessage;\n"
        "import cajeta.net.ws.WsUpgrade;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public final class M {\n"
        "    public static async int32 wsServer(#TcpListener listener) {\n"
        "        TcpStream sock = listener.acceptAsync();\n"
        "        AsyncReader reader = heap AsyncReader(sock);\n"
        "        AsyncWriter writer = heap AsyncWriter(sock);\n"
        "        WebSocket ws = WsUpgrade.acceptServer(reader, writer);\n"
        "        WsMessage m = ws.receive();\n"
        "        int8[] pay = m.getPayload();\n"
        "        int32 plen = m.length();\n"
        "        ws.sendBinary(pay);\n"
        "        sock.close();\n"
        "        return plen;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TcpListener listener = TcpListener.bind(bindAddr);\n"
        "            int32 port = listener.boundPort();\n"
        "            if (port <= 0) { return -1; }\n"
        "            Task<int32> serverTask = spawn wsServer(#listener);\n"
        "            IpAddress ca = IpAddress.loopbackV4();\n"
        "            SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "            TcpStream sock = TcpStream.connectAsync(#connAddr);\n"
        "            AsyncReader reader = heap AsyncReader(sock);\n"
        "            AsyncWriter writer = heap AsyncWriter(sock);\n"
        "            Uri uri = Uri.builder().scheme(\"ws\").host(\"127.0.0.1\").port(port).path(\"/\").build();\n"
        "            WebSocket ws = WsUpgrade.connectClient(reader, writer, uri, 1234567);\n"
        "            int8[] payload = heap int8[4];\n"
        "            payload[0L]=(int8)112; payload[1L]=(int8)105; payload[2L]=(int8)110; payload[3L]=(int8)103;\n"
        "            ws.sendBinary(payload);\n"
        "            WsMessage m = ws.receive();\n"
        "            int32 sret = await serverTask;\n"
        "            int32 mlen = m.length();\n"
        "            int8[] rp = m.getPayload();\n"
        "            sock.close();\n"
        "            if (sret != 4) { return -2; }\n"
        "            if (mlen != 4) { return -3; }\n"
        "            if (rp[0L] != (int8) 112) { return -4; }\n"
        "            return 1;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
