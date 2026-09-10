// cajeta.io.net TLS engine, #included into cajeta_runtime.c (single-TU build).
// Memory-BIO TLS over OpenSSL: it owns no descriptor, the Cajeta layer feeding
// ciphertext into rbio and pulling it from wbio, so no I/O parks in C.

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <string.h>
#include <stdlib.h>

// wincrypt.h #defines identifiers that collide with OpenSSL's types, so it goes
// after the OpenSSL headers and those macros are dropped again below.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#  undef X509_NAME
#  undef X509_EXTENSIONS
#  undef X509_CERT_PAIR
#  undef PKCS7_ISSUER_AND_SERIAL
#  undef PKCS7_SIGNER_INFO
#  undef OCSP_REQUEST
#  undef OCSP_RESPONSE
#endif

// Normalized results: >= 0 is progress (bytes, or 0 = handshake complete).
#define CAJETA_TLS_WANT_IO (-1)
#define CAJETA_TLS_ZERO    (-2)
#define CAJETA_TLS_ERROR   (-3)

// @Native ABI: an int8[] arrives as its CajetaArray header, so buffer pointers
// advance 8 bytes to the data; the explicit length argument is authoritative.
#define CAJETA_ARR_DATA(hdr) ((hdr) ? ((char*) (hdr)) + 8 : (char*) 0)

static int cajeta_tls_alpn_ex_idx = -1;

// Idempotent library init; also registers the ALPN ex-data index above.
static void cajeta_tls_ensure_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                     | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    cajeta_tls_alpn_ex_idx =
        SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

typedef struct {
    SSL* ssl;
    BIO* rbio;   // app feeds ciphertext IN here  (network -> TLS)
    BIO* wbio;   // app pulls ciphertext OUT here  (TLS -> network)
    int  is_server;
} cajeta_tls_conn;

// A server's ALPN list, copied per context and attached to it as ex_data.
typedef struct {
    unsigned char* data;
    unsigned int   len;
} cajeta_tls_alpn_list;

// ---- context (shared config: protocol versions, server cert/key) ----------

// Creates a TLS context with a TLS 1.2 floor; an opaque SSL_CTX*, or NULL.
void* __cajeta_tls_ctx_new(int is_server) {
    cajeta_tls_ensure_init();
    const SSL_METHOD* method = is_server ? TLS_server_method()
                                         : TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    return ctx;
}

// Loads a server certificate chain + private key from in-memory PEM bytes.
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

// Peer-certificate verification: mode != 0 fails the handshake on a bad chain.
int __cajeta_tls_ctx_set_verify(void* ctxv, int mode) {
    SSL_CTX* ctx = (SSL_CTX*) ctxv;
    if (!ctx) return CAJETA_TLS_ERROR;
    SSL_CTX_set_verify(ctx, mode ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    return 0;
}

// Adds every trust anchor in `pem` to the store; 0 if at least one was added.
int __cajeta_tls_ctx_add_trust_pem(void* ctxv, const void* pem_hdr, int len) {
    SSL_CTX* ctx = (SSL_CTX*) ctxv;
    const char* pem = CAJETA_ARR_DATA(pem_hdr);
    if (!ctx || !pem || len <= 0) return CAJETA_TLS_ERROR;
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (!store) return CAJETA_TLS_ERROR;
    BIO* bio = BIO_new_mem_buf(pem, len);
    if (!bio) return CAJETA_TLS_ERROR;
    int added = 0;
    X509* cert;
    while ((cert = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        if (X509_STORE_add_cert(store, cert) == 1) added++;
        X509_free(cert);
    }
    BIO_free(bio);
    return added > 0 ? 0 : CAJETA_TLS_ERROR;
}

// Loads the OS trust store so public certificates validate without a supplied
// anchor: OpenSSL's verify paths on POSIX, the system "ROOT" store on Windows.
int __cajeta_tls_ctx_use_system_trust(void* ctxv) {
    SSL_CTX* ctx = (SSL_CTX*) ctxv;
    if (!ctx) return CAJETA_TLS_ERROR;
#if defined(_WIN32)
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (!store) return CAJETA_TLS_ERROR;
    HCERTSTORE hStore = CertOpenSystemStoreA(0, "ROOT");
    if (!hStore) return CAJETA_TLS_ERROR;
    PCCERT_CONTEXT pctx = NULL;
    int added = 0;
    while ((pctx = CertEnumCertificatesInStore(hStore, pctx)) != NULL) {
        const unsigned char* enc = (const unsigned char*) pctx->pbCertEncoded;
        X509* x = d2i_X509(NULL, &enc, (long) pctx->cbCertEncoded);
        if (x) {
            if (X509_STORE_add_cert(store, x) == 1) added++;
            X509_free(x);
        }
    }
    CertCloseStore(hStore, 0);
    return added > 0 ? 0 : CAJETA_TLS_ERROR;
#else
    return SSL_CTX_set_default_verify_paths(ctx) == 1 ? 0 : CAJETA_TLS_ERROR;
#endif
}

// Frees a context, releasing any server ALPN list attached to it as ex_data.
void __cajeta_tls_ctx_free(void* ctxv) {
    if (!ctxv) return;
    SSL_CTX* ctx = (SSL_CTX*) ctxv;
    if (cajeta_tls_alpn_ex_idx >= 0) {
        cajeta_tls_alpn_list* L =
            (cajeta_tls_alpn_list*) SSL_CTX_get_ex_data(ctx,
                                                        cajeta_tls_alpn_ex_idx);
        if (L) { free(L->data); free(L); }
    }
    SSL_CTX_free(ctx);
}

// ---- connection (per-handshake state + the two memory BIOs) ----------------

// Creates a connection on `ctx` with fresh memory BIOs and connect/accept state.
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

// Sets the client SNI server name from host bytes + length; 0 or an error.
int __cajeta_tls_set_sni(void* connv, const void* host_hdr, int host_len) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    const char* host = CAJETA_ARR_DATA(host_hdr);
    if (!c || !host || host_len <= 0 || host_len > 255) return CAJETA_TLS_ERROR;
    char name[256];
    memcpy(name, host, (size_t) host_len);
    name[host_len] = '\0';
    return SSL_set_tlsext_host_name(c->ssl, name) == 1 ? 0 : CAJETA_TLS_ERROR;
}

// Requires the peer cert to match `host`; set before the handshake it fails.
int __cajeta_tls_set_verify_host(void* connv, const void* host_hdr, int host_len) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    const char* host = CAJETA_ARR_DATA(host_hdr);
    if (!c || !host || host_len <= 0 || host_len > 255) return CAJETA_TLS_ERROR;
    char name[256];
    memcpy(name, host, (size_t) host_len);
    name[host_len] = '\0';
    return SSL_set1_host(c->ssl, name) == 1 ? 0 : CAJETA_TLS_ERROR;
}

// The post-handshake verdict, as a stable ordinal for CertificateInvalid.
#define CAJETA_TLS_CERT_OK        0
#define CAJETA_TLS_CERT_EXPIRED   1
#define CAJETA_TLS_CERT_HOSTNAME  2
#define CAJETA_TLS_CERT_UNTRUSTED 3
#define CAJETA_TLS_CERT_OTHER     4
int __cajeta_tls_verify_result(void* connv) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    if (!c) return CAJETA_TLS_CERT_OTHER;
    long r = SSL_get_verify_result(c->ssl);
    switch (r) {
        case X509_V_OK:
            return CAJETA_TLS_CERT_OK;
        case X509_V_ERR_CERT_HAS_EXPIRED:
        case X509_V_ERR_CERT_NOT_YET_VALID:
        case X509_V_ERR_CRL_HAS_EXPIRED:
            return CAJETA_TLS_CERT_EXPIRED;
        case X509_V_ERR_HOSTNAME_MISMATCH:
        case X509_V_ERR_IP_ADDRESS_MISMATCH:
            return CAJETA_TLS_CERT_HOSTNAME;
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
            return CAJETA_TLS_CERT_UNTRUSTED;
        default:
            return CAJETA_TLS_CERT_OTHER;
    }
}

// Offers an ALPN list in wire format: each entry a 1-byte length then its bytes.
int __cajeta_tls_set_alpn(void* connv, const void* protos_hdr, int len) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    const char* protos = CAJETA_ARR_DATA(protos_hdr);
    if (!c || !protos || len <= 0) return CAJETA_TLS_ERROR;
    // SSL_set_alpn_protos returns 0 on SUCCESS (note the inverted convention).
    return SSL_set_alpn_protos(c->ssl, (const unsigned char*) protos,
                               (unsigned) len) == 0 ? 0 : CAJETA_TLS_ERROR;
}

// Writes the negotiated ALPN protocol (up to `max` bytes) into `out`; 0 if none.
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

// Picks the first server-preferred protocol the client also offered. On no
// overlap it declines with NOACK, leaving `*out` untouched (cf. CVE-2024-5535).
static int cajeta_tls_alpn_select_cb(SSL* ssl, const unsigned char** out,
                                     unsigned char* outlen,
                                     const unsigned char* in, unsigned int inlen,
                                     void* arg) {
    (void) arg;
    if (cajeta_tls_alpn_ex_idx < 0) return SSL_TLSEXT_ERR_NOACK;
    SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
    cajeta_tls_alpn_list* L =
        (cajeta_tls_alpn_list*) SSL_CTX_get_ex_data(ctx, cajeta_tls_alpn_ex_idx);
    if (!L || !L->data || L->len == 0) return SSL_TLSEXT_ERR_NOACK;
    if (SSL_select_next_proto((unsigned char**) out, outlen,
                              L->data, L->len, in, inlen)
            == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

// Installs the ALPN-select callback with `protos`, freeing any previous list. It
// lives on the CONTEXT, read at each handshake, so it may be set after conn_new.
int __cajeta_tls_ctx_set_alpn_select(void* ctxv, const void* protos_hdr,
                                     int len) {
    SSL_CTX* ctx = (SSL_CTX*) ctxv;
    const unsigned char* protos =
        (const unsigned char*) CAJETA_ARR_DATA(protos_hdr);
    if (!ctx || !protos || len <= 0 || cajeta_tls_alpn_ex_idx < 0) {
        return CAJETA_TLS_ERROR;
    }
    cajeta_tls_alpn_list* L =
        (cajeta_tls_alpn_list*) malloc(sizeof(cajeta_tls_alpn_list));
    if (!L) return CAJETA_TLS_ERROR;
    L->data = (unsigned char*) malloc((size_t) len);
    if (!L->data) { free(L); return CAJETA_TLS_ERROR; }
    memcpy(L->data, protos, (size_t) len);
    L->len = (unsigned int) len;

    cajeta_tls_alpn_list* old =
        (cajeta_tls_alpn_list*) SSL_CTX_get_ex_data(ctx, cajeta_tls_alpn_ex_idx);
    if (old) { free(old->data); free(old); }
    SSL_CTX_set_ex_data(ctx, cajeta_tls_alpn_ex_idx, L);
    SSL_CTX_set_alpn_select_cb(ctx, cajeta_tls_alpn_select_cb, NULL);
    return 0;
}

// ---- the memory-BIO pump ---------------------------------------------------

// Feeds network ciphertext in; the bytes consumed (len), or CAJETA_TLS_ERROR.
int __cajeta_tls_feed_ciphertext(void* connv, const void* buf_hdr, int len) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    const char* buf = CAJETA_ARR_DATA(buf_hdr);
    if (!c || (len > 0 && !buf)) return CAJETA_TLS_ERROR;
    if (len == 0) return 0;
    int n = BIO_write(c->rbio, buf, len);
    return n > 0 ? n : CAJETA_TLS_ERROR;
}

int __cajeta_tls_pull_ciphertext(void* connv, void* out_hdr, int max) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    char* out = CAJETA_ARR_DATA(out_hdr);
    if (!c || !out || max <= 0) return 0;
    int n = BIO_read(c->wbio, out, max);
    return n > 0 ? n : 0;
}

int __cajeta_tls_pending_ciphertext(void* connv) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    if (!c) return 0;
    return (int) BIO_ctrl_pending(c->wbio);
}

// Maps SSL_get_error on a non-positive `ret` onto the normalized codes above.
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

// Drives one handshake step: 0 when complete, WANT_IO to exchange and retry.
int __cajeta_tls_handshake_step(void* connv) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    if (!c) return CAJETA_TLS_ERROR;
    int ret = SSL_do_handshake(c->ssl);
    if (ret == 1) return 0;
    return cajeta_tls_classify(c->ssl, ret);
}

// Encrypts `len` plaintext bytes into wbio for the caller to pull.
int __cajeta_tls_write_plaintext(void* connv, const void* buf_hdr, int len) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    const char* buf = CAJETA_ARR_DATA(buf_hdr);
    if (!c || (len > 0 && !buf)) return CAJETA_TLS_ERROR;
    if (len == 0) return 0;
    int n = SSL_write(c->ssl, buf, len);
    if (n > 0) return n;
    return cajeta_tls_classify(c->ssl, n);
}

// Decrypts available application data into `out`; ZERO = peer close-notify.
int __cajeta_tls_read_plaintext(void* connv, void* out_hdr, int max) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    char* out = CAJETA_ARR_DATA(out_hdr);
    if (!c || !out || max <= 0) return CAJETA_TLS_ERROR;
    int n = SSL_read(c->ssl, out, max);
    if (n > 0) return n;
    return cajeta_tls_classify(c->ssl, n);
}

int __cajeta_tls_shutdown(void* connv) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    if (!c) return CAJETA_TLS_ERROR;
    int n = SSL_shutdown(c->ssl);
    return n >= 0 ? 0 : cajeta_tls_classify(c->ssl, n);
}

// Frees a connection; SSL_free releases both BIOs it owns.
void __cajeta_tls_free(void* connv) {
    cajeta_tls_conn* c = (cajeta_tls_conn*) connv;
    if (!c) return;
    if (c->ssl) SSL_free(c->ssl);
    free(c);
}
