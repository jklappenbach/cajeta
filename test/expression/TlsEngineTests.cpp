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
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <cstring>
#include <string>
#include <vector>
#include <cstdint>

// The engine's normalized return codes (see cajeta_tls.c).
#define CAJETA_TLS_WANT_IO (-1)
#define CAJETA_TLS_ZERO    (-2)
#define CAJETA_TLS_ERROR   (-3)

// The engine's buffer functions take int8[] arguments by their CajetaArray
// HEADER ({ i64 count; data }) per the @Native ABI — they advance the pointer
// past the 8-byte header. These tests call the engine directly (no JIT), so
// each buffer is wrapped in that header layout (see Arr below).
extern "C" {
void* __cajeta_tls_ctx_new(int is_server);
int   __cajeta_tls_ctx_use_cert_key_pem(void* ctx, const void* cert_hdr, int cert_len,
                                        const void* key_hdr, int key_len);
void  __cajeta_tls_ctx_free(void* ctx);
void* __cajeta_tls_conn_new(void* ctx, int is_server);
int   __cajeta_tls_ctx_set_verify(void* ctx, int mode);
int   __cajeta_tls_ctx_add_trust_pem(void* ctx, const void* pem_hdr, int len);
int   __cajeta_tls_set_verify_host(void* conn, const void* host_hdr, int host_len);
int   __cajeta_tls_verify_result(void* conn);
int   __cajeta_tls_set_sni(void* conn, const void* host_hdr, int host_len);
int   __cajeta_tls_set_alpn(void* conn, const void* protos_hdr, int len);
int   __cajeta_tls_get_alpn(void* conn, void* out_hdr, int max);
int   __cajeta_tls_feed_ciphertext(void* conn, const void* buf_hdr, int len);
int   __cajeta_tls_pull_ciphertext(void* conn, void* out_hdr, int max);
int   __cajeta_tls_pending_ciphertext(void* conn);
int   __cajeta_tls_handshake_step(void* conn);
int   __cajeta_tls_write_plaintext(void* conn, const void* buf_hdr, int len);
int   __cajeta_tls_read_plaintext(void* conn, void* out_hdr, int max);
int   __cajeta_tls_shutdown(void* conn);
void  __cajeta_tls_free(void* conn);
}

namespace {

// A CajetaArray-header-layout buffer: { i64 count; data[cap] }. hdr() returns
// the header pointer the engine expects; data() points at the element bytes.
struct Arr {
    std::vector<char> buf;
    explicit Arr(int cap) : buf(8 + (cap > 0 ? cap : 0)) {
        *reinterpret_cast<int64_t*>(buf.data()) = cap;
    }
    Arr(const void* src, int len) : buf(8 + len) {
        *reinterpret_cast<int64_t*>(buf.data()) = len;
        if (len > 0) std::memcpy(buf.data() + 8, src, (size_t) len);
    }
    void* hdr() { return buf.data(); }
    char* data() { return buf.data() + 8; }
};

// Mint a self-signed P-256 cert for `cn`, valid [now+beforeSec, now+afterSec]
// (a negative afterSec yields an already-expired cert). Returns PEM cert + key.
bool makeCert(const char* cn, long beforeSec, long afterSec,
              std::string& certPem, std::string& keyPem) {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (!pkey) return false;

    X509* x = X509_new();
    if (!x) { EVP_PKEY_free(pkey); return false; }
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), beforeSec);
    X509_gmtime_adj(X509_getm_notAfter(x), afterSec);
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*) cn, -1, -1, 0);
    X509_set_issuer_name(x, name);   // self-signed: issuer == subject

    // Subject Alternative Name (DNS:<cn>) — what X509_check_host matches first.
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
        BIO_free(cb);
        BIO_free(kb);
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
    return ok;
}

// A standard 1-hour-valid self-signed cert for `cn`.
bool makeSelfSigned(const char* cn, std::string& certPem, std::string& keyPem) {
    return makeCert(cn, 0, 3600, certPem, keyPem);
}

// Build a verifying client (SSL_VERIFY_PEER, optional trust anchor + hostname).
// Returns the conn; *outCtx receives the ctx (caller frees both).
void* makeVerifyingClient(const std::string* trustPem, const char* host,
                          void** outCtx) {
    void* ctx = __cajeta_tls_ctx_new(0);
    __cajeta_tls_ctx_set_verify(ctx, 1);
    if (trustPem) {
        Arr ta(trustPem->data(), (int) trustPem->size());
        __cajeta_tls_ctx_add_trust_pem(ctx, ta.hdr(), (int) trustPem->size());
    }
    void* conn = __cajeta_tls_conn_new(ctx, 0);
    if (host) {
        Arr h(host, (int) std::strlen(host));
        __cajeta_tls_set_verify_host(conn, h.hdr(), (int) std::strlen(host));
    }
    *outCtx = ctx;
    return conn;
}

// Build a server conn presenting cert/key. *outCtx receives the ctx.
void* makeServer(const std::string& cert, const std::string& key, void** outCtx) {
    void* ctx = __cajeta_tls_ctx_new(1);
    Arr c(cert.data(), (int) cert.size());
    Arr k(key.data(), (int) key.size());
    __cajeta_tls_ctx_use_cert_key_pem(ctx, c.hdr(), (int) cert.size(),
                                      k.hdr(), (int) key.size());
    void* conn = __cajeta_tls_conn_new(ctx, 1);
    *outCtx = ctx;
    return conn;
}

// Shuttle all queued ciphertext from `src` into `dst`. Returns bytes moved.
int moveCiphertext(void* src, void* dst) {
    Arr out(16384);
    int total = 0, n;
    while ((n = __cajeta_tls_pull_ciphertext(src, out.hdr(), 16384)) > 0) {
        Arr in(out.data(), n);
        EXPECT_EQ(__cajeta_tls_feed_ciphertext(dst, in.hdr(), n), n);
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
    Arr certArr(cert.data(), (int) cert.size());
    Arr keyArr(key.data(), (int) key.size());
    ASSERT_EQ(__cajeta_tls_ctx_use_cert_key_pem(
                  sctx, certArr.hdr(), (int) cert.size(),
                  keyArr.hdr(), (int) key.size()), 0);
    void* cctx = __cajeta_tls_ctx_new(/*is_server=*/0);
    ASSERT_NE(cctx, nullptr);

    void* server = __cajeta_tls_conn_new(sctx, /*is_server=*/1);
    void* client = __cajeta_tls_conn_new(cctx, /*is_server=*/0);
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);
    Arr sni("localhost", 9);
    __cajeta_tls_set_sni(client, sni.hdr(), 9);

    ASSERT_TRUE(pumpHandshake(client, server)) << "handshake did not complete";

    // client -> server
    const char* req = "GET / HTTP/1.1\r\n\r\n";
    Arr reqArr(req, (int) strlen(req));
    int wn = __cajeta_tls_write_plaintext(client, reqArr.hdr(), (int) strlen(req));
    EXPECT_EQ(wn, (int) strlen(req));
    EXPECT_GT(moveCiphertext(client, server), 0);
    Arr got(256);
    int rn = __cajeta_tls_read_plaintext(server, got.hdr(), 256);
    ASSERT_EQ(rn, (int) strlen(req));
    EXPECT_EQ(std::string(got.data(), (size_t) rn), std::string(req));

    // server -> client
    const char* resp = "HTTP/1.1 200 OK\r\n\r\nhi";
    Arr respArr(resp, (int) strlen(resp));
    wn = __cajeta_tls_write_plaintext(server, respArr.hdr(), (int) strlen(resp));
    EXPECT_EQ(wn, (int) strlen(resp));
    EXPECT_GT(moveCiphertext(server, client), 0);
    Arr got2(256);
    rn = __cajeta_tls_read_plaintext(client, got2.hdr(), 256);
    ASSERT_EQ(rn, (int) strlen(resp));
    EXPECT_EQ(std::string(got2.data(), (size_t) rn), std::string(resp));

    __cajeta_tls_free(client);
    __cajeta_tls_free(server);
    __cajeta_tls_ctx_free(cctx);
    __cajeta_tls_ctx_free(sctx);
}

// An EXPIRED cert is rejected — even when it's the trust anchor and the
// hostname matches, so expiry is the only fault. verify_result == EXPIRED.
TEST(TlsEngineTests, expiredCertRejected) {
    std::string cert, key;
    ASSERT_TRUE(makeCert("localhost", -7200, -3600, cert, key));  // expired 1h ago

    void* sctx; void* server = makeServer(cert, key, &sctx);
    void* cctx; void* client = makeVerifyingClient(&cert, "localhost", &cctx);

    EXPECT_FALSE(pumpHandshake(client, server)) << "expired cert should fail";
    EXPECT_EQ(__cajeta_tls_verify_result(client), 1 /*EXPIRED*/);

    __cajeta_tls_free(client); __cajeta_tls_free(server);
    __cajeta_tls_ctx_free(cctx); __cajeta_tls_ctx_free(sctx);
}

// A cert for `good.test` is rejected when connecting expecting `wrong.test`,
// even though it's trusted + unexpired. verify_result == HOSTNAME.
TEST(TlsEngineTests, hostnameMismatchRejected) {
    std::string cert, key;
    ASSERT_TRUE(makeSelfSigned("good.test", cert, key));

    void* sctx; void* server = makeServer(cert, key, &sctx);
    void* cctx; void* client = makeVerifyingClient(&cert, "wrong.test", &cctx);

    EXPECT_FALSE(pumpHandshake(client, server)) << "hostname mismatch should fail";
    EXPECT_EQ(__cajeta_tls_verify_result(client), 2 /*HOSTNAME*/);

    __cajeta_tls_free(client); __cajeta_tls_free(server);
    __cajeta_tls_ctx_free(cctx); __cajeta_tls_ctx_free(sctx);
}

// A self-signed cert from an untrusted root is rejected; adding it as a trust
// anchor then makes the same handshake succeed.
TEST(TlsEngineTests, untrustedRootRejectedThenAcceptedWithAnchor) {
    std::string cert, key;
    ASSERT_TRUE(makeSelfSigned("localhost", cert, key));

    // (a) no trust anchor -> rejected as untrusted.
    {
        void* sctx; void* server = makeServer(cert, key, &sctx);
        void* cctx; void* client = makeVerifyingClient(nullptr, "localhost", &cctx);
        EXPECT_FALSE(pumpHandshake(client, server)) << "untrusted root should fail";
        EXPECT_EQ(__cajeta_tls_verify_result(client), 3 /*UNTRUSTED*/);
        __cajeta_tls_free(client); __cajeta_tls_free(server);
        __cajeta_tls_ctx_free(cctx); __cajeta_tls_ctx_free(sctx);
    }
    // (b) trust the cert as an anchor -> accepted.
    {
        void* sctx; void* server = makeServer(cert, key, &sctx);
        void* cctx; void* client = makeVerifyingClient(&cert, "localhost", &cctx);
        EXPECT_TRUE(pumpHandshake(client, server)) << "trusted anchor should succeed";
        EXPECT_EQ(__cajeta_tls_verify_result(client), 0 /*OK*/);
        __cajeta_tls_free(client); __cajeta_tls_free(server);
        __cajeta_tls_ctx_free(cctx); __cajeta_tls_ctx_free(sctx);
    }
}
