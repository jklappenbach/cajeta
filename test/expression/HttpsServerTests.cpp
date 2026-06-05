// NET-9.5 acceptance — the HTTP/1.1 server over TLS termination, end-to-end on
// a loopback socket.
//
// A spawned server fiber binds a TlsListener (self-signed localhost cert),
// accepts + completes the server TLS handshake, then runs the *real* HttpServer
// keep-alive loop (HttpServer.serveTlsStream) over the encrypted ByteChannel —
// TLS is pure termination, the HTTP protocol code is unchanged. The HttpClient
// (NET-8.1) makes an https GET; the handler returns 200 "hello"; the client
// verifies the cert against the pinned anchor and parses the body. Proves the
// server half runs over TLS (the client half is HttpClientTests).

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
    std::string s = "        int8[] " + name + " = new int8["
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

// HTTPS request end-to-end: client (HttpClient, NET-8.1) -> server (HttpServer
// over TLS via serveTlsStream, NET-9.5). The handler echoes a fixed body; the
// client gets 200 + "hello".
//
// This was DISABLED on Bug 2: the server loop reads/writes through
// AsyncReader/AsyncWriter, whose `ByteChannel stream` field is a FORWARD-
// REFERENCED interface that the compiler had laid out as a thin class pointer
// instead of a fat interface pointer (the interface flag wasn't set until the
// interface's own declaration was visited, after AsyncReader's field layout),
// so `this.stream.readAsync(...)` interface dispatch was silently dropped at
// codegen. Bug 2 is now FIXED (prescan g_interfaceArchive marks interfaces ->
// fromContext synthesizes a born-fat interface placeholder ->
// visitInterfaceDeclaration reuses it so the method set lands on the captured
// object; see memory `cajeta-interface-arg-field-offset-bug`). This HTTPS live
// row — the full HttpServer keep-alive loop over a TLS-terminated ByteChannel,
// under a spawned server fiber — is green: it is the end-to-end proof that the
// forward-referenced interface field now dispatches correctly over a live
// socket. TLS itself is also covered by TlsLoopbackTest; the plaintext HTTP
// CLIENT path is green in HttpClientTests.
TEST(HttpsServerTests, httpsRequestEndToEnd) {
    std::string cert, key;
    ASSERT_TRUE(makeSelfSigned("localhost", cert, key));
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.IpAddress;\n"
        "import cajeta.net.SocketAddress;\n"
        "import cajeta.net.tls.TlsStream;\n"
        "import cajeta.net.tls.TlsListener;\n"
        "import cajeta.net.uri.Uri;\n"
        "import cajeta.net.http.HttpClient;\n"
        "import cajeta.net.http.HttpServer;\n"
        "import cajeta.net.http.HttpRequest;\n"
        "import cajeta.net.http.HttpResponse;\n"
        "import cajeta.net.http.HttpParserLimits;\n"
        "import cajeta.net.http.ServerLimits;\n"
        "import cajeta.threading.Tasks;\n"
        "public final class M {\n"
        "    public static #HttpResponse handle(HttpRequest req) {\n"
        "        String s = \"hello\";\n"
        "        return HttpResponse.ok().body(s.bytes, s.byteLength);\n"
        "    }\n"
        "    public static async int32 httpsServer(#TlsListener listener) {\n"
        "        TlsStream s = listener.acceptAsync();\n"
        "        (HttpRequest) -> #HttpResponse handler = (HttpRequest req) -> M.handle(req);\n"
        "        HttpParserLimits lim = heap HttpParserLimits();\n"
        "        ServerLimits slim = heap ServerLimits();\n"
        "        HttpServer.serveTlsStream(handler, lim, slim, #s);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        + emitBytes("cert", cert)
        + emitBytes("key", key) +
        "            int32 cl = " + std::to_string(cert.size()) + ";\n"
        "            int32 kl = " + std::to_string(key.size()) + ";\n"
        "            IpAddress la = IpAddress.loopbackV4();\n"
        "            SocketAddress bindAddr = SocketAddress.of(#la, 0);\n"
        "            TlsListener listener = TlsListener.bind(bindAddr, cert, cl, key, kl);\n"
        "            int32 port = listener.boundPort();\n"
        "            if (port <= 0) { return -1; }\n"
        "            Task<int32> serverTask = spawn httpsServer(#listener);\n"
        "            HttpClient client = heap HttpClient();\n"
        "            client.trustAnchor(cert, cl);\n"
        "            Uri uri = Uri.builder().scheme(\"https\").host(\"localhost\").port(port).path(\"/\").build();\n"
        "            HttpRequest req = HttpRequest.fromUri(\"GET\", uri);\n"
        "            HttpResponse resp = client.send(uri, req);\n"
        "            int32 sret = await serverTask;\n"
        "            int32 status = resp.statusCode();\n"
        "            int32 blen = resp.bodyLength();\n"
        "            if (status != 200) { return -2; }\n"
        "            if (blen != 5) { return -3; }\n"
        "            int8[] rb = resp.body;\n"
        "            if (rb[0L] != (int8) 104) { return -4; }\n"   // 'h'
        "            if (rb[4L] != (int8) 111) { return -5; }\n"   // 'o'
        "            return 1;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
