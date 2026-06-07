// NET-5.2 acceptance — a real TLS handshake over a loopback TCP socket.
//
// The socket-facing half of TlsClient: a client TlsStream and a spawned server
// fiber complete a TLS 1.3 handshake over a loopback connection (driving the
// memory-BIO pump against TcpStream.readAsync/writeAllAsync, parking on the
// reactor), the client verifies the server's cert against a supplied anchor +
// hostname, then a plaintext message echoes back. Mirrors NetAsyncEchoTest's
// fiber+loopback shape (b3), with TLS layered on and the server on its own fiber
// (a handshake is multi-round + bidirectional, so it needs real concurrency).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

bool makeSelfSigned(const char* cn, std::string& certPem, std::string& keyPem) {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (!pkey) return false;
    X509* x = X509_new();
    if (!x) { EVP_PKEY_free(pkey); return false; }
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600L);
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*) cn, -1, -1, 0);
    X509_set_issuer_name(x, name);
    std::string san = std::string("DNS:") + cn;
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name,
                                              san.c_str());
    if (ext) { X509_add_ext(x, ext, -1); X509_EXTENSION_free(ext); }
    bool ok = X509_sign(x, pkey, EVP_sha256()) != 0;
    if (ok) {
        BIO* cb = BIO_new(BIO_s_mem());
        BIO* kb = BIO_new(BIO_s_mem());
        ok = PEM_write_bio_X509(cb, x) &&
             PEM_write_bio_PrivateKey(kb, pkey, NULL, NULL, 0, NULL, NULL);
        if (ok) {
            char* p; long n;
            n = BIO_get_mem_data(cb, &p); certPem.assign(p, (size_t) n);
            n = BIO_get_mem_data(kb, &p); keyPem.assign(p, (size_t) n);
        }
        BIO_free(cb); BIO_free(kb);
    }
    X509_free(x); EVP_PKEY_free(pkey);
    return ok;
}

std::string emitBytes(const std::string& name, const std::string& data) {
    std::string s = "        int8[] " + name + " = heap int8["
                  + std::to_string(data.size()) + "];\n";
    for (size_t i = 0; i < data.size(); i++) {
        s += "        " + name + "[" + std::to_string(i) + "L] = (int8) "
           + std::to_string((int) (signed char) data[i]) + ";\n";
    }
    return s;
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Isolation probe: the same spawn + loopback shape WITHOUT TLS — a spawned
// server fiber echoes raw bytes. Pins whether the spawn+socket+fiber pattern
// itself works in the JIT, independent of the TLS layer.
TEST(TlsLoopbackTest, spawnedServerLoopbackEchoNoTls) {
    std::string src =
        "package test;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.TcpListener;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class M {\n"
        "    public static async int32 echoServer(#TcpStream sock, int8[] tag) {\n"
        "        int8[] b = heap int8[4];\n"
        "        int64 n = sock.readAsync(b, (int64) 0, (int64) 4);\n"
        "        sock.writeAllAsync(b, (int64) 0, n);\n"
        "        return (int32) tag[0L];\n"   // use the int8[] spawn arg
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TcpListener listener = TcpListener.bind(bindAddr);\n"
        "            int32 port = listener.boundPort();\n"
        "            IpAddress ca = IpAddress.loopbackV4();\n"
        "            SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "            TcpStream client = TcpStream.connect(#connAddr);\n"
        "            TcpStream server = listener.accept();\n"
        "            int8[] tag = heap int8[2];\n"
        "            tag[0L] = (int8) 7; tag[1L] = (int8) 9;\n"
        "            Task<int32> t = spawn echoServer(#server, tag);\n"
        "            int8[] ping = heap int8[4];\n"
        "            ping[0L]=(int8)112; ping[1L]=(int8)105; ping[2L]=(int8)110; ping[3L]=(int8)103;\n"
        "            client.writeAsync(ping, (int64) 0, (int64) 4);\n"
        "            int8[] echo = heap int8[4];\n"
        "            int64 g = client.readAsync(echo, (int64) 0, (int64) 4);\n"
        "            int32 sr = await t;\n"
        "            listener.close();\n"
        "            if (sr != 7) { return -5; }\n"   // the int8[] spawn arg round-tripped
        "            if (g != (int64) 4) { return -2; }\n"
        "            if (echo[0L] != ping[0L]) { return -3; }\n"
        "            return 1;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Isolation: construct a client TlsStream over a real connected socket in the
// fiber, but DON'T handshake (server just accepts). Pins TlsStream construction
// + the #-moves of TcpStream/TlsConnection into fields, apart from the pump.
TEST(TlsLoopbackTest, clientTlsStreamConstructNoHandshake) {
    std::string cert, key;
    ASSERT_TRUE(makeSelfSigned("localhost", cert, key));
    std::string src =
        "package test;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.TcpListener;\n"
        "import cajeta.net.tls.TlsStream;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class M {\n"
        "    public static async int32 acceptOnly(#TcpStream sock) {\n"
        "        return 1;\n"   // hold the accepted stream, then drop it
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        + emitBytes("cert", cert) +
        "            int8[] host = heap int8[9];\n"
        "            host[0L]=(int8)108; host[1L]=(int8)111; host[2L]=(int8)99;\n"
        "            host[3L]=(int8)97; host[4L]=(int8)108; host[5L]=(int8)104;\n"
        "            host[6L]=(int8)111; host[7L]=(int8)115; host[8L]=(int8)116;\n"
        "            int32 cl = " + std::to_string(cert.size()) + ";\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TcpListener listener = TcpListener.bind(bindAddr);\n"
        "            int32 port = listener.boundPort();\n"
        "            IpAddress ca = IpAddress.loopbackV4();\n"
        "            SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "            TcpStream client = TcpStream.connect(#connAddr);\n"
        "            TcpStream server = listener.accept();\n"
        "            Task<int32> t = spawn acceptOnly(#server);\n"
        "            TlsStream ct = TlsStream.client(#client, host, 9, cert, cl);\n"
        "            int32 sr = await t;\n"
        "            listener.close();\n"
        "            return 1;\n"   // ct + everything drops here
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Scheduler regression guard (cross-carrier fiber pinning). A spawned fiber
// #-moves a TcpStream (whose ~TcpStream dtor runs on the fiber's stack) into a
// heap holder, then drops it at scope exit — no socket I/O. Before the
// home-carrier pinning fix this SIGSEGV'd deterministically on a >=4-carrier
// pool: a started fiber could be work-stolen and resumed on a different carrier
// than the one its ucontext was bound to, corrupting its stack mid-teardown.
// (Only reproduces when the pool has more carriers than fibers, i.e. >=4 cores;
// harmless but always-green elsewhere.)
TEST(TlsLoopbackTest, crossCarrierFiberHolderDropRegression) {
    std::string src =
        "package test;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.TcpListener;\n"
        "import cajeta.threading.Tasks;\n"
        "public class Pipe {\n"
        "    TcpStream stream;\n"
        "    private Pipe(#TcpStream s) { this.stream = #s; }\n"
        "    public static #Pipe wrap(#TcpStream s) { return heap Pipe(#s); }\n"
        "}\n"
        "public final class M {\n"
        "    public static async int32 runServer(#TcpStream sock) {\n"
        "        Pipe p = Pipe.wrap(#sock);\n"
        "        return 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TcpListener listener = TcpListener.bind(bindAddr);\n"
        "            int32 port = listener.boundPort();\n"
        "            IpAddress ca = IpAddress.loopbackV4();\n"
        "            SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "            TcpStream client = TcpStream.connect(#connAddr);\n"
        "            TcpStream server = listener.accept();\n"
        "            Task<int32> t = spawn runServer(#server);\n"
        "            int32 sr = await t;\n"
        "            listener.close();\n"
        "            return sr;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// A full TLS 1.3 handshake + plaintext echo over a loopback socket, with the
// client and server each driving the memory-BIO pump on its own fiber.
//
// This originally SIGSEGV'd, and the root cause was NOT in the TLS layer: the
// fiber stack was 64 KB, but a TLS handshake runs a deep OpenSSL call tree
// (SSL_do_handshake → X.509 chain parse → ECDSA P-256 → ASN.1) that overflowed
// it. Fiber stacks are malloc'd with no guard page, so the overflow corrupted
// adjacent heap and surfaced as a crash (full speed) or a wedged scheduler
// (under gdb). Fixed by sizing fiber stacks like an OS thread stack (1 MB,
// $CAJETA_FIBER_STACK_KB-overridable) in cajeta_runtime.c. The in-memory TLS
// tests passed throughout because they run on the main thread's large stack.
TEST(TlsLoopbackTest, tlsHandshakeAndEchoOverLoopback) {
    std::string cert, key;
    ASSERT_TRUE(makeSelfSigned("localhost", cert, key));

    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.TcpListener;\n"
        "import cajeta.net.tls.TlsStream;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class M {\n"
        // server fiber: TLS-wrap the accepted stream, handshake, echo one record.
        "    public static async int32 runServer(#TcpStream sock, int8[] cert, int32 certLen,\n"
        "                                         int8[] key, int32 keyLen) {\n"
        "        TlsStream st = TlsStream.server(#sock, cert, certLen, key, keyLen);\n"
        "        st.handshake();\n"
        "        int8[] inbuf = heap int8[256];\n"
        "        int32 n = st.read(inbuf, 256);\n"
        "        st.write(inbuf, n);\n"
        "        return n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        + emitBytes("cert", cert)
        + emitBytes("key", key) +
        "            int8[] host = heap int8[9];\n"   // "localhost"
        "            host[0L]=(int8)108; host[1L]=(int8)111; host[2L]=(int8)99;\n"
        "            host[3L]=(int8)97; host[4L]=(int8)108; host[5L]=(int8)104;\n"
        "            host[6L]=(int8)111; host[7L]=(int8)115; host[8L]=(int8)116;\n"
        "            int32 cl = " + std::to_string(cert.size()) + ";\n"
        "            int32 kl = " + std::to_string(key.size()) + ";\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TcpListener listener = TcpListener.bind(bindAddr);\n"
        "            int32 port = listener.boundPort();\n"
        "            if (port <= 0) { return -1; }\n"
        "            IpAddress ca = IpAddress.loopbackV4();\n"
        "            SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "            TcpStream client = TcpStream.connect(#connAddr);\n"
        "            TcpStream server = listener.accept();\n"
        "            Task<int32> serverTask = spawn runServer(#server, cert, cl, key, kl);\n"
        "            TlsStream ct = TlsStream.client(#client, host, 9, cert, cl);\n"
        "            ct.handshake();\n"
        "            int8[] ping = heap int8[4];\n"
        "            ping[0L]=(int8)112; ping[1L]=(int8)105; ping[2L]=(int8)110; ping[3L]=(int8)103;\n"
        "            ct.write(ping, 4);\n"
        "            int8[] echo = heap int8[256];\n"
        "            int32 got = ct.read(echo, 256);\n"
        "            int32 sret = await serverTask;\n"
        "            listener.close();\n"
        "            if (sret != 4) { return -2; }\n"
        "            if (got != 4) { return -3; }\n"
        "            int32 i = 0;\n"
        "            while (i < 4) {\n"
        "                if (echo[(int64) i] != ping[(int64) i]) { return -4; }\n"
        "                i = i + 1;\n"
        "            }\n"
        "            return 1;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";

    EXPECT_EQ(runI32(src), 1);
}

// NET-5.5 + NET-5.4 acceptance — server-side TLS via TlsListener, with ALPN
// negotiated end-to-end over the loopback socket. A spawned server fiber
// TlsListener.accept()s (which terminates TLS — accept + handshake), reads one
// record and echoes it; the client TlsStream offers ["h2","http/1.1"], the
// listener supports only "http/1.1", so both negotiate "http/1.1".
TEST(TlsLoopbackTest, tlsListenerAcceptWithAlpnOverLoopback) {
    std::string cert, key;
    ASSERT_TRUE(makeSelfSigned("localhost", cert, key));
    std::string serverProtos = std::string("\x08", 1) + "http/1.1";          // 9
    std::string clientProtos = std::string("\x02", 1) + "h2" +
                               std::string("\x08", 1) + "http/1.1";          // 12

    std::string src =
        "package test;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.TcpStream;\n"
        "import cajeta.net.tls.TlsStream;\n"
        "import cajeta.net.tls.TlsListener;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class M {\n"
        "    public static async int32 serveOne(#TlsListener listener) {\n"
        "        TlsStream s = listener.accept();\n"   // accept + server handshake
        "        int8[] buf = heap int8[256];\n"
        "        int32 n = s.read(buf, 256);\n"
        "        s.write(buf, n);\n"
        "        return n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        + emitBytes("cert", cert)
        + emitBytes("key", key)
        + emitBytes("sproto", serverProtos)
        + emitBytes("cproto", clientProtos) +
        "            int8[] host = heap int8[9];\n"   // \"localhost\"
        "            host[0L]=(int8)108; host[1L]=(int8)111; host[2L]=(int8)99;\n"
        "            host[3L]=(int8)97; host[4L]=(int8)108; host[5L]=(int8)104;\n"
        "            host[6L]=(int8)111; host[7L]=(int8)115; host[8L]=(int8)116;\n"
        "            int32 cl = " + std::to_string(cert.size()) + ";\n"
        "            int32 kl = " + std::to_string(key.size()) + ";\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TlsListener listener = TlsListener.bind(bindAddr, cert, cl, key, kl);\n"
        "            listener.supportAlpn(sproto, 9);\n"
        "            int32 port = listener.boundPort();\n"
        "            if (port <= 0) { return -1; }\n"
        "            IpAddress ca = IpAddress.loopbackV4();\n"
        "            SocketAddress connAddr = SocketAddress.of(#ca, port);\n"
        "            TcpStream client = TcpStream.connect(#connAddr);\n"
        "            Task<int32> serverTask = spawn serveOne(#listener);\n"
        "            TlsStream ct = TlsStream.client(#client, host, 9, cert, cl);\n"
        "            ct.offerAlpn(cproto, 12);\n"
        "            ct.handshake();\n"
        "            int8[] neg = heap int8[64];\n"
        "            int32 nlen = ct.negotiatedAlpn(neg, 64);\n"
        "            int8[] ping = heap int8[4];\n"
        "            ping[0L]=(int8)112; ping[1L]=(int8)105; ping[2L]=(int8)110; ping[3L]=(int8)103;\n"
        "            ct.write(ping, 4);\n"
        "            int8[] echo = heap int8[256];\n"
        "            int32 got = ct.read(echo, 256);\n"
        "            int32 sret = await serverTask;\n"
        "            if (sret != 4) { return -2; }\n"
        "            if (got != 4) { return -3; }\n"
        "            if (nlen != 8) { return -4; }\n"             // \"http/1.1\" len
        "            if (neg[0L] != (int8) 104) { return -6; }\n"  // 'h'
        "            if (neg[7L] != (int8) 49) { return -7; }\n"   // '1'
        "            int32 i = 0;\n"
        "            while (i < 4) {\n"
        "                if (echo[(int64) i] != ping[(int64) i]) { return -5; }\n"
        "                i = i + 1;\n"
        "            }\n"
        "            return 1;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";

    EXPECT_EQ(runI32(src), 1);
}
