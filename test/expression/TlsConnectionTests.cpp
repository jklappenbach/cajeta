// NET-5.2 — cajeta.io.net.tls.TlsConnection driven through the JIT.
//
// Validates the full Cajeta TLS surface end-to-end: a client and a server
// TlsConnection (created via the @Native engine bindings) complete a handshake
// purely by shuttling ciphertext between each other through int8[] buffers — no
// socket — then plaintext round-trips. This is the pure-logic pump the
// socket-facing TlsClient (over AsyncReader/AsyncWriter) runs one level up.
//
// The server cert is an ephemeral self-signed EC cert minted in-test and
// emitted into the Cajeta source as int8[] byte literals (the same data-literal
// shape NetResolveTests uses for host bytes).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <openssl/evp.h>
#include <openssl/x509.h>
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

// Emit Cajeta source declaring `int8[] name` initialized to `data`'s bytes.
std::string emitBytes(const std::string& name, const std::string& data) {
    std::string s = "        int8[] " + name + " = heap int8["
                  + std::to_string(data.size()) + "];\n";
    for (size_t i = 0; i < data.size(); i++) {
        s += "        " + name + "[" + std::to_string(i) + "L] = (int8) "
           + std::to_string((int) (signed char) data[i]) + ";\n";
    }
    return s;
}

// Pump a verifying client against a server, returning client.verifyResult().
// When `trust` is set the client adds the server cert as a trust anchor first.
std::string verifyBody(const std::string& cert, const std::string& key, bool trust) {
    std::string b =
        emitBytes("cert", cert) +
        emitBytes("key", key) +
        "        int8[] host = heap int8[9];\n"                       // "localhost"
        "        host[0L]=(int8)108; host[1L]=(int8)111; host[2L]=(int8)99;\n"
        "        host[3L]=(int8)97; host[4L]=(int8)108; host[5L]=(int8)104;\n"
        "        host[6L]=(int8)111; host[7L]=(int8)115; host[8L]=(int8)116;\n"
        "        TlsConnection client = TlsConnection.verifyingClient();\n";
    if (trust) {
        b += "        client.addTrust(cert, " + std::to_string(cert.size()) + ");\n";
    }
    b +=
        "        client.setVerifyHost(host, 9);\n"
        "        TlsConnection server = TlsConnection.server(cert, " + std::to_string(cert.size()) +
        ", key, " + std::to_string(key.size()) + ");\n"
        "        int8[] buf = heap int8[16384];\n"
        "        int32 round = 0;\n"
        "        boolean stop = false;\n"
        "        while (round < 64 && !stop) {\n"
        "            int32 cs = client.handshakeStep();\n"
        "            int32 n = client.pull(buf, 16384);\n"
        "            while (n > 0) { server.feed(buf, n); n = client.pull(buf, 16384); }\n"
        "            int32 ss = server.handshakeStep();\n"
        "            n = server.pull(buf, 16384);\n"
        "            while (n > 0) { client.feed(buf, n); n = server.pull(buf, 16384); }\n"
        "            if (cs == TlsConnection.FAILED) { stop = true; }\n"
        "            if (cs == 0 && ss == 0) { stop = true; }\n"
        "            round = round + 1;\n"
        "        }\n"
        "        return client.verifyResult();\n";
    return b;
}

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.io.net.tls.TlsConnection;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// A client + server TlsConnection complete a handshake over int8[] buffers,
// then a plaintext message round-trips client -> server.
TEST(TlsConnectionTests, handshakeAndPlaintextThroughCajetaSurface) {
    std::string cert, key;
    ASSERT_TRUE(makeSelfSigned("localhost", cert, key));

    std::string body =
        emitBytes("cert", cert) +
        emitBytes("key", key) +
        "        TlsConnection client = TlsConnection.client();\n"
        "        TlsConnection server = TlsConnection.server(cert, " + std::to_string(cert.size()) +
        ", key, " + std::to_string(key.size()) + ");\n"
        // pump the handshake
        "        int8[] buf = heap int8[16384];\n"
        "        int32 round = 0;\n"
        "        boolean done = false;\n"
        "        while (round < 64 && !done) {\n"
        "            int32 cs = client.handshakeStep();\n"
        "            int32 n = client.pull(buf, 16384);\n"
        "            while (n > 0) { server.feed(buf, n); n = client.pull(buf, 16384); }\n"
        "            int32 ss = server.handshakeStep();\n"
        "            n = server.pull(buf, 16384);\n"
        "            while (n > 0) { client.feed(buf, n); n = server.pull(buf, 16384); }\n"
        "            if (cs == TlsConnection.FAILED) { return -2; }\n"
        "            if (ss == TlsConnection.FAILED) { return -3; }\n"
        "            if (cs == 0 && ss == 0) { done = true; }\n"
        "            round = round + 1;\n"
        "        }\n"
        "        if (!done) { return -4; }\n"
        // plaintext round-trip: client -> server
        "        int8[] msg = heap int8[2];\n"
        "        msg[0L] = (int8) 104;\n"   // 'h'
        "        msg[1L] = (int8) 105;\n"   // 'i'
        "        int32 wn = client.write(msg, 2);\n"
        "        if (wn != 2) { return -5; }\n"
        "        int32 m = client.pull(buf, 16384);\n"
        "        while (m > 0) { server.feed(buf, m); m = client.pull(buf, 16384); }\n"
        "        int8[] got = heap int8[256];\n"
        "        int32 rn = server.read(got, 256);\n"
        "        if (rn != 2) { return -6; }\n"
        "        if (got[0L] != (int8) 104) { return -7; }\n"
        "        if (got[1L] != (int8) 105) { return -8; }\n"
        "        return 1;\n";

    EXPECT_EQ(runI32(body), 1);
}

// Cert validation through the Cajeta surface: a verifying client rejects an
// untrusted self-signed cert (CERT_UNTRUSTED) but accepts it once trusted
// (CERT_OK).
TEST(TlsConnectionTests, verifyingClientRejectsUntrustedAcceptsTrusted) {
    std::string cert, key;
    ASSERT_TRUE(makeSelfSigned("localhost", cert, key));
    EXPECT_EQ(runI32(verifyBody(cert, key, /*trust=*/false)), 3);  // CERT_UNTRUSTED
    EXPECT_EQ(runI32(verifyBody(cert, key, /*trust=*/true)), 0);   // CERT_OK
}
