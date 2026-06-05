// cajeta.net TLS engine — NET-5.1.
//
// Memory-BIO TLS over a portable backend (OpenSSL's libssl here; the same
// SSL_*/BIO_* surface BoringSSL forked from, so a later static-BoringSSL swap
// is mechanical — see plan Phase 5 § Design decision). The engine owns NO file
// descriptor: a connection is an `SSL*` wired to two memory BIOs —
//   - rbio  (network -> TLS): the Cajeta layer feeds ciphertext it read off the
//            socket here, via __cajeta_tls_feed_ciphertext;
//   - wbio  (TLS -> network): the engine writes ciphertext the Cajeta layer
//            pulls out (via __cajeta_tls_pull_ciphertext) and writes to the
//            socket.
// That decoupling is what lets TLS sit on the async reactor (NET-3.x): the
// handshake/read/write are pure state transitions over the BIOs; all I/O
// parking happens in the Cajeta `TlsClient` pump (NET-5.2), never in C.
//
// Return-code convention (normalized, platform/library constants stay here):
//   handshake_step / read_plaintext / write_plaintext:
//     >= 0  progress (bytes for read/write; 0 = handshake complete for step)
//     CAJETA_TLS_WANT_IO (-1)   need more ciphertext fed / pulled, retry later
//     CAJETA_TLS_ZERO    (-2)   clean close-notify (read EOF)
//     CAJETA_TLS_ERROR   (-3)   fatal protocol/library error
//
// This file is #included into cajeta_runtime.c so it lands in BOTH the embedded
// JIT bitcode and the native test object; libssl/libcrypto are linked by the
// build (src/CMakeLists.txt) so the SSL_* externs resolve at JIT + AOT link.

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <string.h>
#include <stdlib.h>

#define CAJETA_TLS_WANT_IO (-1)
#define CAJETA_TLS_ZERO    (-2)
#define CAJETA_TLS_ERROR   (-3)

// @Native ABI: an int8[] argument arrives as its CajetaArray HEADER
// ({ i64 count; data... }), so every buffer pointer below is advanced past the
// 8-byte header to reach the element data — the same convention __cajeta_sha256_
// update and the getaddrinfo bridges follow. The explicit length arg is the
// authoritative byte count; the header's count field is not re-read here.
#define CAJETA_ARR_DATA(hdr) ((hdr) ? ((char*) (hdr)) + 8 : (char*) 0)

// One-time library init. OpenSSL 3.x auto-inits on first use, but doing it
// explicitly (and idempotently) keeps the engine self-contained and avoids
// relying on lazy-init ordering under the JIT.
static void cajeta_tls_ensure_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                     | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
}

typedef struct {
    SSL* ssl;
    BIO* rbio;   // app feeds ciphertext IN here  (network -> TLS)
    BIO* wbio;   // app pulls ciphertext OUT here  (TLS -> network)
    int  is_server;
} cajeta_tls_conn;

// ---- context (shared config: protocol versions, server cert/key) ----------

// Create a TLS context. `is_server` selects the method; both default to a
// TLS 1.2 floor (1.3 negotiated when both peers support it). Returns an opaque
// SSL_CTX* (NULL on failure).
void* __cajeta_tls_ctx_new(int is_server) {
    cajeta_tls_ensure_init();
    const SSL_METHOD* method = is_server ? TLS_server_method()
                                         : TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    return ctx;
}

// Server-side: load a certificate (chain) + private key from in-memory PEM
// bytes (int8[] headers). Returns 0 on success, CAJETA_TLS_ERROR otherwise.
int __cajeta_tls_ctx_use_cert_key_pem(void* ctxv,
                                      const void* cert_hdr, int cert_len,
                                      const void* key_hdr, int key_len) {
    SSL_CTX* ctx = (SSL_CTX*) ctxv;
    const char* cert_pem = CAJETA_ARR_DATA(cert_hdr);
    const char* key_pem = CAJETA_ARR_DATA(key_hdr);
    if (!ctx || !cert_pem || !key_pem) return CAJETA_TLS_ERROR;

    BIO* cbio = BIO_new_mem_buf(cert_pem, cert_len);
    if (!cbio) return CAJETA_TLS_ERROR;
    X509* cert = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
    BIO_free(cbio);
    if (!cert) return CAJETA_TLS_ERROR;
    int ok = SSL_CTX_use_certificate(ctx, cert);
    X509_free(cert);
    if (ok != 1) return CAJETA_TLS_ERROR;

    BIO* kbio = BIO_new_mem_buf(key_pem, key_len);
    if (!kbio) return CAJETA_TLS_ERROR;
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(kbio, NULL, NULL, NULL);
    BIO_free(kbio);
    if (!pkey) return CAJETA_TLS_ERROR;
    ok = SSL_CTX_use_PrivateKey(ctx, pkey);
    EVP_PKEY_free(pkey);
    if (ok != 1) return CAJETA_TLS_ERROR;

    return SSL_CTX_check_private_key(ctx) == 1 ? 0 : CAJETA_TLS_ERROR;
}

void __cajeta_tls_ctx_free(void* ctxv) {
    if (ctxv) SSL_CTX_free((SSL_CTX*) ctxv);
}

// ---- connection (per-handshake state + the two memory BIOs) ----------------

// Create a connection on `ctx`. Wires fresh memory BIOs and sets connect/accept
// state. Returns an opaque cajeta_tls_conn* (NULL on failure).
void* __cajeta_tls_conn_new(void* ctxv, int is_server) {
    SSL_CTX* ctx = (SSL_CTX*) ctxv;
    if (!ctx) return NULL;
    cajeta_tls_conn* c = (cajeta_tls_conn*) calloc(1, sizeof(cajeta_tls_conn));
    if (!c) return NULL;
    c->ssl = SSL_new(ctx);
    if (!c->ssl) { free(c); return NULL; }
    c->rbio = BIO_new(BIO_s_mem());
    c->wbio = BIO_new(BIO_s_mem());
    if (!c->rbio || !c->wbio) {
        if (c->rbio) BIO_free(c->rbio);
        if (c->wbio) BIO_free(c->wbio);
        SSL_free(c->ssl);
        free(c);
        return NULL;
    }
    // SSL takes ownership of both BIOs (freed by SSL_free).
    SSL_set_bio(c->ssl, c->rbio, c->wbio);
    c->is_server = is_server;
    if (is_server) SSL_set_accept_state(c->ssl);
    else           SSL_set_connect_state(c->ssl);
    return c;
}

// Client-side SNI: the server name to request (also used for hostname
// verification at the Cajeta layer), passed as host bytes (int8[]) + length.
// Copied into a NUL-terminated buffer for the OpenSSL API. 0 / CAJETA_TLS_ERROR.
int __cajeta_tls_set_sni(void* connv, const void* host_hdr, int host_len) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    const char* host = CAJETA_ARR_DATA(host_hdr);
    if (!c || !host || host_len <= 0 || host_len > 255) return CAJETA_TLS_ERROR;
    char name[256];
    memcpy(name, host, (size_t) host_len);
    name[host_len] = '\0';
    return SSL_set_tlsext_host_name(c->ssl, name) == 1 ? 0 : CAJETA_TLS_ERROR;
}

// Offer an ALPN protocol list (wire format: each entry is a 1-byte length
// followed by that many bytes, e.g. "\x08http/1.1"). Returns 0 / error.
int __cajeta_tls_set_alpn(void* connv, const void* protos_hdr, int len) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    const char* protos = CAJETA_ARR_DATA(protos_hdr);
    if (!c || !protos || len <= 0) return CAJETA_TLS_ERROR;
    // SSL_set_alpn_protos returns 0 on SUCCESS (note the inverted convention).
    return SSL_set_alpn_protos(c->ssl, (const unsigned char*) protos,
                               (unsigned) len) == 0 ? 0 : CAJETA_TLS_ERROR;
}

// The negotiated ALPN protocol after handshake. Writes up to `max` bytes into
// `out` and returns the length, or 0 if none negotiated.
int __cajeta_tls_get_alpn(void* connv, void* out_hdr, int max) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    char* out = CAJETA_ARR_DATA(out_hdr);
    if (!c || !out) return 0;
    const unsigned char* proto = NULL;
    unsigned int plen = 0;
    SSL_get0_alpn_selected(c->ssl, &proto, &plen);
    if (!proto || plen == 0) return 0;
    int n = (int) plen < max ? (int) plen : max;
    memcpy(out, proto, (size_t) n);
    return n;
}

// ---- the memory-BIO pump ---------------------------------------------------

// Feed ciphertext received from the network into the TLS engine.
// Returns bytes consumed (== len on success) or CAJETA_TLS_ERROR.
int __cajeta_tls_feed_ciphertext(void* connv, const void* buf_hdr, int len) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    const char* buf = CAJETA_ARR_DATA(buf_hdr);
    if (!c || (len > 0 && !buf)) return CAJETA_TLS_ERROR;
    if (len == 0) return 0;
    int n = BIO_write(c->rbio, buf, len);
    return n > 0 ? n : CAJETA_TLS_ERROR;
}

// Pull ciphertext the engine wants written to the network. Returns the number
// of bytes copied into `out` (0 if none pending).
int __cajeta_tls_pull_ciphertext(void* connv, void* out_hdr, int max) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    char* out = CAJETA_ARR_DATA(out_hdr);
    if (!c || !out || max <= 0) return 0;
    int n = BIO_read(c->wbio, out, max);
    return n > 0 ? n : 0;
}

// How many ciphertext bytes are queued to write to the network.
int __cajeta_tls_pending_ciphertext(void* connv) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    if (!c) return 0;
    return (int) BIO_ctrl_pending(c->wbio);
}

// Map an SSL_get_error result on a non-positive ret into our normalized codes.
static int cajeta_tls_classify(SSL* ssl, int ret) {
    int err = SSL_get_error(ssl, ret);
    switch (err) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return CAJETA_TLS_WANT_IO;
        case SSL_ERROR_ZERO_RETURN:
            return CAJETA_TLS_ZERO;
        default:
            return CAJETA_TLS_ERROR;
    }
}

// Drive one handshake step. Returns 0 when the handshake is complete,
// CAJETA_TLS_WANT_IO when more ciphertext must be exchanged (pull from wbio,
// write to peer; feed peer's bytes into rbio; retry), CAJETA_TLS_ERROR on a
// fatal fault.
int __cajeta_tls_handshake_step(void* connv) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    if (!c) return CAJETA_TLS_ERROR;
    int ret = SSL_do_handshake(c->ssl);
    if (ret == 1) return 0;
    return cajeta_tls_classify(c->ssl, ret);
}

// Encrypt + queue `len` plaintext bytes (the ciphertext lands in wbio for the
// caller to pull). Returns bytes written, CAJETA_TLS_WANT_IO, or error.
int __cajeta_tls_write_plaintext(void* connv, const void* buf_hdr, int len) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    const char* buf = CAJETA_ARR_DATA(buf_hdr);
    if (!c || (len > 0 && !buf)) return CAJETA_TLS_ERROR;
    if (len == 0) return 0;
    int n = SSL_write(c->ssl, buf, len);
    if (n > 0) return n;
    return cajeta_tls_classify(c->ssl, n);
}

// Decrypt available application data into `out`. Returns bytes read,
// CAJETA_TLS_WANT_IO (feed more ciphertext), CAJETA_TLS_ZERO (peer sent
// close-notify), or CAJETA_TLS_ERROR.
int __cajeta_tls_read_plaintext(void* connv, void* out_hdr, int max) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    char* out = CAJETA_ARR_DATA(out_hdr);
    if (!c || !out || max <= 0) return CAJETA_TLS_ERROR;
    int n = SSL_read(c->ssl, out, max);
    if (n > 0) return n;
    return cajeta_tls_classify(c->ssl, n);
}

// Initiate a clean shutdown (queues close-notify into wbio).
int __cajeta_tls_shutdown(void* connv) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    if (!c) return CAJETA_TLS_ERROR;
    int n = SSL_shutdown(c->ssl);
    return n >= 0 ? 0 : cajeta_tls_classify(c->ssl, n);
}

// Free a connection (SSL_free releases both BIOs it owns).
void __cajeta_tls_free(void* connv) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    if (!c) return;
    if (c->ssl) SSL_free(c->ssl);
    free(c);
}
