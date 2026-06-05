// NET-5.1 — the cajeta.net TLS engine (memory-BIO intrinsics) driven natively.
//
// Mirrors NetResolveTests' posture for getaddrinfo: exercise the
// `__cajeta_tls_*` C surface directly (it's linked into the test binary via
// libcajeta_lib), here with NO socket at all — a client and a server SSL object
// wired only through the engine's memory BIOs. Pins the contract the Cajeta
// `TlsClient` pump (NET-5.2) builds on: a handshake completes purely by shuttling
// ciphertext between the two BIO pairs, then plaintext round-trips both ways.
//
// The server cert is an ephemeral self-signed EC cert minted in-test (cert
// VALIDATION is NET-5.3; here the client runs verify-none, so the engine itself
// is what's under test).

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <cstring>
#include <string>

// The engine's normalized return codes (see cajeta_tls.c).
#define CAJETA_TLS_WANT_IO (-1)
#define CAJETA_TLS_ZERO    (-2)
#define CAJETA_TLS_ERROR   (-3)

extern "C" {
void* __cajeta_tls_ctx_new(int is_server);
int   __cajeta_tls_ctx_use_cert_key_pem(void* ctx, const char* cert_pem, int cert_len,
                                        const char* key_pem, int key_len);
void  __cajeta_tls_ctx_free(void* ctx);
void* __cajeta_tls_conn_new(void* ctx, int is_server);
int   __cajeta_tls_set_sni(void* conn, const char* host);
int   __cajeta_tls_set_alpn(void* conn, const char* protos, int len);
int   __cajeta_tls_get_alpn(void* conn, char* out, int max);
int   __cajeta_tls_feed_ciphertext(void* conn, const char* buf, int len);
int   __cajeta_tls_pull_ciphertext(void* conn, char* out, int max);
int   __cajeta_tls_pending_ciphertext(void* conn);
int   __cajeta_tls_handshake_step(void* conn);
int   __cajeta_tls_write_plaintext(void* conn, const char* buf, int len);
int   __cajeta_tls_read_plaintext(void* conn, char* out, int max);
int   __cajeta_tls_shutdown(void* conn);
void  __cajeta_tls_free(void* conn);
}

namespace {

// Mint a throwaway self-signed P-256 cert for `cn`, returning PEM cert + key.
bool makeSelfSigned(const char* cn, std::string& certPem, std::string& keyPem) {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (!pkey) return false;

    X509* x = X509_new();
    if (!x) { EVP_PKEY_free(pkey); return false; }
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 60L * 60L);   // valid 1h
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*) cn, -1, -1, 0);
    X509_set_issuer_name(x, name);   // self-signed: issuer == subject
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
        BIO_free(cb);
        BIO_free(kb);
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
    return ok;
}

// Shuttle all queued ciphertext from `src` into `dst`. Returns bytes moved.
int moveCiphertext(void* src, void* dst) {
    char buf[16384];
    int total = 0, n;
    while ((n = __cajeta_tls_pull_ciphertext(src, buf, sizeof buf)) > 0) {
        EXPECT_EQ(__cajeta_tls_feed_ciphertext(dst, buf, n), n);
        total += n;
    }
    return total;
}

// Drive both peers' handshakes to completion over the memory BIOs.
bool pumpHandshake(void* client, void* server) {
    for (int round = 0; round < 64; ++round) {
        int cs = __cajeta_tls_handshake_step(client);
        moveCiphertext(client, server);
        int ss = __cajeta_tls_handshake_step(server);
        moveCiphertext(server, client);
        if (cs == CAJETA_TLS_ERROR || ss == CAJETA_TLS_ERROR) return false;
        if (cs == 0 && ss == 0) return true;
    }
    return false;
}

} // namespace

// A full handshake completes purely over the memory BIOs, then a plaintext
// message round-trips client -> server -> client.
TEST(TlsEngineTests, memoryBioHandshakeAndPlaintextRoundTrip) {
    std::string cert, key;
    ASSERT_TRUE(makeSelfSigned("localhost", cert, key));

    void* sctx = __cajeta_tls_ctx_new(/*is_server=*/1);
    ASSERT_NE(sctx, nullptr);
    ASSERT_EQ(__cajeta_tls_ctx_use_cert_key_pem(
                  sctx, cert.data(), (int) cert.size(),
                  key.data(), (int) key.size()), 0);
    void* cctx = __cajeta_tls_ctx_new(/*is_server=*/0);
    ASSERT_NE(cctx, nullptr);

    void* server = __cajeta_tls_conn_new(sctx, /*is_server=*/1);
    void* client = __cajeta_tls_conn_new(cctx, /*is_server=*/0);
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);
    __cajeta_tls_set_sni(client, "localhost");

    ASSERT_TRUE(pumpHandshake(client, server)) << "handshake did not complete";

    // client -> server
    const char* req = "GET / HTTP/1.1\r\n\r\n";
    int wn = __cajeta_tls_write_plaintext(client, req, (int) strlen(req));
    EXPECT_EQ(wn, (int) strlen(req));
    EXPECT_GT(moveCiphertext(client, server), 0);
    char got[256];
    int rn = __cajeta_tls_read_plaintext(server, got, sizeof got);
    ASSERT_EQ(rn, (int) strlen(req));
    EXPECT_EQ(std::string(got, (size_t) rn), std::string(req));

    // server -> client
    const char* resp = "HTTP/1.1 200 OK\r\n\r\nhi";
    wn = __cajeta_tls_write_plaintext(server, resp, (int) strlen(resp));
    EXPECT_EQ(wn, (int) strlen(resp));
    EXPECT_GT(moveCiphertext(server, client), 0);
    rn = __cajeta_tls_read_plaintext(client, got, sizeof got);
    ASSERT_EQ(rn, (int) strlen(resp));
    EXPECT_EQ(std::string(got, (size_t) rn), std::string(resp));

    __cajeta_tls_free(client);
    __cajeta_tls_free(server);
    __cajeta_tls_ctx_free(cctx);
    __cajeta_tls_ctx_free(sctx);
}
