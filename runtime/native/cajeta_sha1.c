// SHA-1 (FIPS 180-4) — WebSocket handshake only.
//
// **Not for security use.** SHA-1's collision resistance is broken
// (SHAttered, 2017) and chosen-prefix collisions are practical
// (2020). The *only* sanctioned use in this tree is computing the
// WebSocket `Sec-WebSocket-Accept` value — RFC 6455 §1.3 fixes the
// algorithm as SHA-1(key + magic GUID), so the handshake's security
// does not rest on SHA-1's collision resistance. Do not reach for
// this for signatures, MACs, fingerprinting, or anything an attacker
// can benefit from forging. Use `cajeta.hash.Sha256` instead.
//
// This file is a single translation unit `#include`d from
// cajeta_runtime.c so it shares the runtime's headers (stdint.h,
// string.h, malloc) and gets embedded in the runtime bitcode + the
// native test object without a separate compile target. It defines
// no symbols other than the `__cajeta_sha1_*` C ABI bridges and a
// handful of file-static helpers.
//
// Streaming state, allocator, finalizer, and the width-named Hasher
// primitive folders mirror the MD5 bridges in cajeta_runtime.c so the
// cajeta-side `Sha1` class is shaped identically to `MD5`. The output
// digest is 20 bytes (160 bits); the big-endian byte order is what
// distinguishes the SHA family's serialization from MD5's
// little-endian.

struct cajeta_sha1_state {
    uint32_t h[5];          // H0..H4
    uint64_t bits;          // total bytes hashed * 8
    uint8_t  buf[64];       // partial-block buffer
    int32_t  buf_len;
};

static inline uint32_t sha1_rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32u - n));
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    // SHA-1 ingests each 512-bit block as 16 big-endian words.
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t) block[i*4 + 0] << 24)
             | ((uint32_t) block[i*4 + 1] << 16)
             | ((uint32_t) block[i*4 + 2] << 8)
             | ((uint32_t) block[i*4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = sha1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    uint32_t a = state[0], b = state[1], c = state[2],
             d = state[3], e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotl(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1_init(struct cajeta_sha1_state* s) {
    s->h[0] = 0x67452301u;
    s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xc3d2e1f0u;
    s->bits = 0;
    s->buf_len = 0;
}

static void sha1_update(struct cajeta_sha1_state* s,
                        const uint8_t* data, size_t len) {
    s->bits += (uint64_t) len * 8u;
    while (len > 0) {
        size_t to_copy = (size_t) (64 - s->buf_len);
        if (to_copy > len) to_copy = len;
        memcpy(s->buf + s->buf_len, data, to_copy);
        s->buf_len += (int32_t) to_copy;
        data += to_copy;
        len  -= to_copy;
        if (s->buf_len == 64) {
            sha1_transform(s->h, s->buf);
            s->buf_len = 0;
        }
    }
}

static void sha1_finalize(struct cajeta_sha1_state* s, uint8_t out[20]) {
    // Append 0x80, pad with zeros to 56 mod 64, append the 8-byte
    // BIG-endian bit count, transform.
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) {
        memset(s->buf + s->buf_len, 0, (size_t)(64 - s->buf_len));
        sha1_transform(s->h, s->buf);
        s->buf_len = 0;
    }
    memset(s->buf + s->buf_len, 0, (size_t)(56 - s->buf_len));
    for (int i = 0; i < 8; i++) {
        s->buf[63 - i] = (uint8_t)(s->bits >> (i * 8));
    }
    sha1_transform(s->h, s->buf);
    for (int i = 0; i < 5; i++) {
        out[i*4 + 0] = (uint8_t)(s->h[i] >> 24);
        out[i*4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i*4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i*4 + 3] = (uint8_t)(s->h[i] >> 0);
    }
}

// --- SHA-1 C ABI bridges ---------------------------------------------------
// Streaming state — opaque to cajeta. Allocator + finalizer match the
// ctor / destructor pattern the cajeta Sha1 class uses, mirroring the
// MD5 bridges.

void* __cajeta_sha1_alloc(void) {
    struct cajeta_sha1_state* s = (struct cajeta_sha1_state*) malloc(sizeof *s);
    if (!s) return NULL;
    sha1_init(s);
    return s;
}

void __cajeta_sha1_free(void* state) {
    if (state) free(state);
}

void __cajeta_sha1_reset(void* state) {
    if (state) sha1_init((struct cajeta_sha1_state*) state);
}

// `data_hdr` is a cajeta int8[] header — { i64 count, [N x i8] data }.
// Caller passes the explicit `len`; bytes are read from offset 8.
void __cajeta_sha1_update(void* state, const void* data_hdr, int64_t len) {
    if (!state || !data_hdr || len <= 0) return;
    const uint8_t* data = ((const uint8_t*) data_hdr) + 8;
    sha1_update((struct cajeta_sha1_state*) state, data, (size_t) len);
}

// out_hdr is a cajeta int8[20] header. Writes 20 bytes at offset 8.
void __cajeta_sha1_finalize_into(void* state, void* out_hdr) {
    if (!state || !out_hdr) return;
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    sha1_finalize((struct cajeta_sha1_state*) state, out);
}

// Width-named primitive folders — match MD5's behavior (little-endian
// byte serialization of each scalar) so the Hasher contract is uniform
// across algorithms. NB: SHA-1's *message* serialization is big-endian
// internally, but the Hasher.write* contract pins the byte sequence the
// algorithm ingests, and that contract is defined as the value's
// little-endian bytes (identical to MD5/SipHash) so a caller swapping
// algorithms sees the same input stream.
void __cajeta_sha1_write_i8 (void* state, int8_t  v) {
    if (state) sha1_update((struct cajeta_sha1_state*) state, (const uint8_t*) &v, 1);
}
void __cajeta_sha1_write_i16(void* state, int16_t v) {
    if (!state) return;
    uint8_t b[2] = { (uint8_t)(v), (uint8_t)(v >> 8) };
    sha1_update((struct cajeta_sha1_state*) state, b, 2);
}
void __cajeta_sha1_write_i32(void* state, int32_t v) {
    if (!state) return;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
    sha1_update((struct cajeta_sha1_state*) state, b, 4);
}
void __cajeta_sha1_write_i64(void* state, int64_t v) {
    if (!state) return;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    sha1_update((struct cajeta_sha1_state*) state, b, 8);
}
void __cajeta_sha1_write_f32(void* state, float v) {
    if (!state) return;
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_sha1_write_i32(state, (int32_t) bits);
}
void __cajeta_sha1_write_f64(void* state, double v) {
    if (!state) return;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_sha1_write_i64(state, (int64_t) bits);
}
void __cajeta_sha1_write_bool(void* state, int8_t v) {
    __cajeta_sha1_write_i8(state, v ? 1 : 0);
}

// finish() Hasher projection: return the first 8 digest bytes as a
// little-endian int64. Mutates the state (calls sha1_finalize), so a
// second finish() returns garbage — Hasher.finish() is terminal.
int64_t __cajeta_sha1_finish_int64(void* state) {
    if (!state) return 0;
    uint8_t digest[20];
    sha1_finalize((struct cajeta_sha1_state*) state, digest);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t) digest[i]) << (i * 8);
    }
    return (int64_t) v;
}

// One-shot variants. Caller pre-allocates the output array on the
// cajeta side (since @Native return of int8[] isn't ABI-bridged in
// v1). These fill the caller's buffer at `out_hdr + 8`.
void __cajeta_sha1_oneshot_into(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    struct cajeta_sha1_state s;
    sha1_init(&s);
    if (data_hdr && len > 0) {
        sha1_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    sha1_finalize(&s, ((uint8_t*) out_hdr) + 8);
}

// Lowercase 40-char hex digest into a caller-supplied int8[40] buffer.
void __cajeta_sha1_oneshot_hex_into(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    struct cajeta_sha1_state s;
    sha1_init(&s);
    if (data_hdr && len > 0) {
        sha1_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    uint8_t digest[20];
    sha1_finalize(&s, digest);
    static const char HEX[16] = "0123456789abcdef";
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    for (int i = 0; i < 20; i++) {
        out[i*2 + 0] = (uint8_t) HEX[(digest[i] >> 4) & 0xF];
        out[i*2 + 1] = (uint8_t) HEX[digest[i] & 0xF];
    }
}
