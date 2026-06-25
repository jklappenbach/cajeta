// SHA-256 (FIPS 180-4) native runtime primitives for `cajeta.hash.Sha256`.
//
// This file is **#include'd** into `cajeta_runtime.c` near the MD5
// section — it is NOT a standalone translation unit. The runtime is
// compiled as a single TU to one LLVM bitcode module that the codegen
// embeds + Linker-merges into JIT output; a separate .o would never be
// seen by `@Native` symbol resolution. Including here keeps SHA-256 in
// the same module as MD5 / SipHash / XXHash3, matching how every other
// `cajeta.hash` algorithm is wired. All <stdint.h>/<string.h> headers
// the body needs are already included by cajeta_runtime.c above.
//
// The C ABI mirrors the MD5 bridges one-for-one (alloc/free/reset/
// update/finalize_into/finish_int64/write_iN/write_fN/oneshot_into/
// oneshot_hex_into) so the cajeta Sha256 class is a structural twin of
// MD5 — same field-index + live-set workarounds. Differences from MD5:
// the digest is 32 bytes (not 16), the word order is **big-endian**
// (SHA-2 is big-endian; MD5 is little-endian), and the padding length
// field is a 64-bit big-endian bit count.
//
// Cross-checked against the FIPS 180-4 worked examples + NIST CAVP
// short-message vectors (empty / "abc" / the 56-byte two-block
// message). See test/parser/Sha256Tests.cpp for the pinned vectors.

struct cajeta_sha256_state {
    uint32_t s[8];          // H0..H7
    uint64_t bits;          // total bytes hashed * 8
    uint8_t  buf[64];       // partial-block buffer
    int32_t  buf_len;
};

// First 32 bits of the fractional parts of the cube roots of the first
// 64 primes (FIPS 180-4 §4.2.2).
static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

// ── Hardware SHA path (x86 SHA-NI: sha256rnds2/msg1/msg2) ───────────────────
// The crown-holders (rust sha2, OpenSSL, Go crypto/sha256) all dispatch to the
// CPU SHA extensions, hitting ~2.0-2.4 GB/s; a scalar transform tops out near
// ~0.5 GB/s. We do the same: a SHA-NI multi-block transform gated by a one-shot
// CPUID probe, with the scalar transform as the fallback (non-x86, or x86
// without SHA). The probe uses inline `cpuid` (<cpuid.h>) — no external
// __cpu_model/__cpu_indicator_init symbols, which the LLJIT can't materialize.
#if defined(__x86_64__) || defined(__i386__)
#define CAJETA_SHA256_X86 1
#include <immintrin.h>
#include <cpuid.h>

static int cajeta_cpu_has_sha(void) {
    static int v = -1;
    if (v >= 0) return v;
    unsigned a, b, c, d;
    int has = 0;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) has = (b & (1u << 29)) != 0;
    v = has;
    return v;
}

// SHA-NI multi-block transform. Message schedule kept in four rotating 128-bit
// windows (verified against the scalar schedule); two sha256rnds2 per 4-round
// group. Big-endian message load via the byte-shuffle MASK. The full unroll
// keeps the windows in registers (array indices are compile-time constants).
__attribute__((target("sha,sse4.1,ssse3")))
static void sha256_shani(uint32_t state[8], const uint8_t* data, size_t blocks) {
    const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i STATE0, STATE1, MSG, ABEF_SAVE, CDGH_SAVE;
    __m128i TMP = _mm_loadu_si128((const __m128i*)&state[0]);
    STATE1      = _mm_loadu_si128((const __m128i*)&state[4]);
    TMP    = _mm_shuffle_epi32(TMP, 0xB1);
    STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);
    STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0);

    while (blocks) {
        ABEF_SAVE = STATE0; CDGH_SAVE = STATE1;
        __m128i M[4];
        M[0] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data+0)),  MASK);
        M[1] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data+16)), MASK);
        M[2] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data+32)), MASK);
        M[3] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data+48)), MASK);
        #pragma clang loop unroll(full)
        for (int g = 0; g < 16; g++) {
            __m128i cur = M[g & 3];
            MSG = _mm_add_epi32(cur, _mm_loadu_si128((const __m128i*)&SHA256_K[g*4]));
            STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
            // Produce the window for group g+4 while the first rnds2 retires.
            if (g < 12) {
                __m128i o = M[g & 3], a = M[(g+1) & 3], b = M[(g+2) & 3], c = M[(g+3) & 3];
                o = _mm_sha256msg1_epu32(o, a);
                o = _mm_add_epi32(o, _mm_alignr_epi8(c, b, 4));
                M[g & 3] = _mm_sha256msg2_epu32(o, c);
            }
            MSG = _mm_shuffle_epi32(MSG, 0x0E);
            STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        }
        STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
        STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);
        data += 64; blocks--;
    }

    TMP    = _mm_shuffle_epi32(STATE0, 0x1B);
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0);
    STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);
    _mm_storeu_si128((__m128i*)&state[0], STATE0);
    _mm_storeu_si128((__m128i*)&state[4], STATE1);
}
#else
static int cajeta_cpu_has_sha(void) { return 0; }
#endif

static void sha256_transform_scalar(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    // Message schedule: first 16 words are the block read big-endian.
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t) block[i*4 + 0] << 24)
             | ((uint32_t) block[i*4 + 1] << 16)
             | ((uint32_t) block[i*4 + 2] << 8)
             | ((uint32_t) block[i*4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(W[i-15], 7) ^ sha256_rotr(W[i-15], 18)
                    ^ (W[i-15] >> 3);
        uint32_t s1 = sha256_rotr(W[i-2], 17) ^ sha256_rotr(W[i-2], 19)
                    ^ (W[i-2] >> 10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + SHA256_K[i] + W[i];
        uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

// Process `blocks` complete 64-byte blocks, dispatching to SHA-NI when the CPU
// advertises the SHA extensions, else the scalar transform.
static void sha256_blocks(uint32_t state[8], const uint8_t* data, size_t blocks) {
#ifdef CAJETA_SHA256_X86
    if (cajeta_cpu_has_sha()) { sha256_shani(state, data, blocks); return; }
#endif
    for (; blocks; blocks--, data += 64) sha256_transform_scalar(state, data);
}

static void sha256_init(struct cajeta_sha256_state* s) {
    // First 32 bits of the fractional parts of the square roots of the
    // first 8 primes (FIPS 180-4 §5.3.3).
    s->s[0] = 0x6a09e667;
    s->s[1] = 0xbb67ae85;
    s->s[2] = 0x3c6ef372;
    s->s[3] = 0xa54ff53a;
    s->s[4] = 0x510e527f;
    s->s[5] = 0x9b05688c;
    s->s[6] = 0x1f83d9ab;
    s->s[7] = 0x5be0cd19;
    s->bits = 0;
    s->buf_len = 0;
}

static void sha256_update(struct cajeta_sha256_state* s,
                          const uint8_t* data, size_t len) {
    s->bits += (uint64_t) len * 8u;
    // 1. Top off any partial buffer to a full block first.
    if (s->buf_len > 0) {
        size_t need = (size_t) (64 - s->buf_len);
        size_t take = need < len ? need : len;
        memcpy(s->buf + s->buf_len, data, take);
        s->buf_len += (int32_t) take;
        data += take; len -= take;
        if (s->buf_len == 64) { sha256_blocks(s->s, s->buf, 1); s->buf_len = 0; }
    }
    // 2. Hash whole blocks straight from the caller's buffer — no per-block
    //    memcpy, and SHA-NI keeps the digest state in registers across them.
    if (len >= 64) {
        size_t nb = len >> 6;
        sha256_blocks(s->s, data, nb);
        data += nb << 6; len -= nb << 6;
    }
    // 3. Stash the sub-block tail for next time.
    if (len > 0) {
        memcpy(s->buf, data, len);
        s->buf_len = (int32_t) len;
    }
}

static void sha256_finalize(struct cajeta_sha256_state* s, uint8_t out[32]) {
    // Append 0x80, pad with zeros to 56 mod 64, append the 8-byte
    // big-endian bit count, transform.
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) {
        memset(s->buf + s->buf_len, 0, (size_t)(64 - s->buf_len));
        sha256_blocks(s->s, s->buf, 1);
        s->buf_len = 0;
    }
    memset(s->buf + s->buf_len, 0, (size_t)(56 - s->buf_len));
    for (int i = 0; i < 8; i++) {
        s->buf[56 + i] = (uint8_t)(s->bits >> ((7 - i) * 8));
    }
    sha256_blocks(s->s, s->buf, 1);
    for (int i = 0; i < 8; i++) {
        out[i*4 + 0] = (uint8_t)(s->s[i] >> 24);
        out[i*4 + 1] = (uint8_t)(s->s[i] >> 16);
        out[i*4 + 2] = (uint8_t)(s->s[i] >> 8);
        out[i*4 + 3] = (uint8_t)(s->s[i] >> 0);
    }
}

// --- SHA-256 C ABI bridges -------------------------------------------------
// Streaming state — opaque to cajeta. Allocator + finalizer match the
// destructor / ctor pattern the cajeta Sha256 class uses (mirrors MD5).

void* __cajeta_sha256_alloc(void) {
    struct cajeta_sha256_state* s = (struct cajeta_sha256_state*) malloc(sizeof *s);
    if (!s) return NULL;
    sha256_init(s);
    return s;
}

void __cajeta_sha256_free(void* state) {
    if (state) free(state);
}

void __cajeta_sha256_reset(void* state) {
    if (state) sha256_init((struct cajeta_sha256_state*) state);
}

// `data_hdr` is a cajeta int8[] header — { i64 count, [N x i8] data }.
// Caller passes the explicit `len`; bytes are read from offset 8.
void __cajeta_sha256_update(void* state, const void* data_hdr, int64_t len) {
    if (!state || !data_hdr || len <= 0) return;
    const uint8_t* data = ((const uint8_t*) data_hdr) + 8;
    sha256_update((struct cajeta_sha256_state*) state, data, (size_t) len);
}

// out_hdr is a cajeta int8[32] header. Writes 32 bytes starting at
// offset 8. Caller sizes the array.
void __cajeta_sha256_finalize_into(void* state, void* out_hdr) {
    if (!state || !out_hdr) return;
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    sha256_finalize((struct cajeta_sha256_state*) state, out);
}

// Width-named primitive folders — little-endian byte representation,
// matching the MD5 Hasher contract (so swapping algorithm classes keeps
// the same byte sequence per primitive).
void __cajeta_sha256_write_i8 (void* state, int8_t  v) {
    if (state) sha256_update((struct cajeta_sha256_state*) state, (const uint8_t*) &v, 1);
}
void __cajeta_sha256_write_i16(void* state, int16_t v) {
    if (!state) return;
    uint8_t b[2] = { (uint8_t)(v), (uint8_t)(v >> 8) };
    sha256_update((struct cajeta_sha256_state*) state, b, 2);
}
void __cajeta_sha256_write_i32(void* state, int32_t v) {
    if (!state) return;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
    sha256_update((struct cajeta_sha256_state*) state, b, 4);
}
void __cajeta_sha256_write_i64(void* state, int64_t v) {
    if (!state) return;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    sha256_update((struct cajeta_sha256_state*) state, b, 8);
}
void __cajeta_sha256_write_f32(void* state, float v) {
    if (!state) return;
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_sha256_write_i32(state, (int32_t) bits);
}
void __cajeta_sha256_write_f64(void* state, double v) {
    if (!state) return;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_sha256_write_i64(state, (int64_t) bits);
}
void __cajeta_sha256_write_bool(void* state, int8_t v) {
    __cajeta_sha256_write_i8(state, v ? 1 : 0);
}

// finish() Hasher projection: first 8 digest bytes as a little-endian
// int64. Mutates the state (calls sha256_finalize), so a second finish()
// returns garbage — Hasher.finish() is terminal by contract.
int64_t __cajeta_sha256_finish_int64(void* state) {
    if (!state) return 0;
    uint8_t digest[32];
    sha256_finalize((struct cajeta_sha256_state*) state, digest);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t) digest[i]) << (i * 8);
    }
    return (int64_t) v;
}

// One-shot variants. Caller pre-allocates the output array on the cajeta
// side (since @Native return of int8[] isn't ABI-bridged in v1). These
// helpers fill the caller's buffer at `out_hdr + 8`.
void __cajeta_sha256_oneshot_into(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    struct cajeta_sha256_state s;
    sha256_init(&s);
    if (data_hdr && len > 0) {
        sha256_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    sha256_finalize(&s, ((uint8_t*) out_hdr) + 8);
}

// Lowercase hex digest into a caller-supplied int8[64] buffer.
void __cajeta_sha256_oneshot_hex_into(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    struct cajeta_sha256_state s;
    sha256_init(&s);
    if (data_hdr && len > 0) {
        sha256_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    uint8_t digest[32];
    sha256_finalize(&s, digest);
    static const char HEX[16] = "0123456789abcdef";
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    for (int i = 0; i < 32; i++) {
        out[i*2 + 0] = (uint8_t) HEX[(digest[i] >> 4) & 0xF];
        out[i*2 + 1] = (uint8_t) HEX[digest[i] & 0xF];
    }
}

// Finalize a streaming state into a caller-supplied int8[64] hex buffer.
// Lets the cajeta `Sha256.digestHex()` / `hex()` surface produce hex
// without a separate raw-digest round-trip.
void __cajeta_sha256_finalize_hex_into(void* state, void* out_hdr) {
    if (!state || !out_hdr) return;
    uint8_t digest[32];
    sha256_finalize((struct cajeta_sha256_state*) state, digest);
    static const char HEX[16] = "0123456789abcdef";
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    for (int i = 0; i < 32; i++) {
        out[i*2 + 0] = (uint8_t) HEX[(digest[i] >> 4) & 0xF];
        out[i*2 + 1] = (uint8_t) HEX[digest[i] & 0xF];
    }
}
