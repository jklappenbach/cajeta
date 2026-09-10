// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- general-purpose hashing (cajeta.hash backend) --------------------------
// The primitives behind Object.hash() and cajeta.hash.*: SplitMix64 for primitive
// values, XXH3-64 for bytes, both XOR'd with a per-process seed from the CSPRNG.

#include <fcntl.h>
#include <unistd.h>
#include <time.h>

// XXH_INLINE_ALL: bitcode and native builds carry it; no libxxhash link step.
#define XXH_INLINE_ALL
#include <xxhash.h>

static uint64_t __cajeta_hash_seed_value = 0;

__attribute__((constructor))
static void __cajeta_hash_seed_init(void) {
    uint64_t s = 0;
#if defined(_WIN32)
    // BCryptGenRandom is Windows' /dev/urandom; SYSTEM_PREFERRED needs no handle.
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
    if (BCryptGenRandom(NULL, (PUCHAR) &s, sizeof(s),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 /* STATUS_SUCCESS */
            && s != 0) {
        __cajeta_hash_seed_value = s;
        return;
    }
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, &s, sizeof(s));
        close(fd);
        if (n == (ssize_t) sizeof(s) && s != 0) {
            __cajeta_hash_seed_value = s;
            return;
        }
    }
#endif
    // Fallback: wall clock + pid through SplitMix64. Still per-process distinct.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t x = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
#if defined(_WIN32)
    x ^= (uint64_t) GetCurrentProcessId() * 0x9E3779B97F4A7C15ULL;
#else
    x ^= (uint64_t) getpid() * 0x9E3779B97F4A7C15ULL;
#endif
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    __cajeta_hash_seed_value = x ? x : 0x9E3779B97F4A7C15ULL;
}

// Lazy seed accessor. The constructor above runs only in the NATIVE build; a
// JIT-loaded copy has its own static that no .init_array touches.
static inline uint64_t __cajeta_hash_seed_load(void) {
    uint64_t s = __cajeta_hash_seed_value;
    if (__builtin_expect(s == 0, 0)) {
        __cajeta_hash_seed_init();
        s = __cajeta_hash_seed_value;
    }
    return s;
}

// Exposed as cajeta.hash.Hash.processSeed(), to align with Object.hash() values.
int64_t __cajeta_hash_seed(void) {
    return (int64_t) __cajeta_hash_seed_load();
}

// SplitMix64 finalizer — the mixer behind every primitive hash variant.
static inline uint64_t splitmix64_finalize(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// A cheaper integer mix than splitmix64_finalize, still a BIJECTION on uint64, so
// distinct keys hash distinctly. `>> 29` folds mixed high bits into SwissTable's.
static inline uint64_t fast_int_mix(uint64_t x) {
    x *= 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29;
    return x;
}

int64_t __cajeta_hash_int64(int64_t value) {
    return (int64_t) fast_int_mix((uint64_t) value ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_int32(int32_t value) {
    // Sign-extend, so all-ones int32 does not hash like ~0 int64.
    return (int64_t) fast_int_mix(
        (uint64_t) (int64_t) value ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_float64(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    // -0 canonicalizes to +0 (IEEE); each NaN bit pattern keeps a distinct hash.
    if (bits == 0x8000000000000000ULL) bits = 0;
    return (int64_t) splitmix64_finalize(bits ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_float32(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if (bits == 0x80000000U) bits = 0;
    return (int64_t) splitmix64_finalize((uint64_t) bits ^ __cajeta_hash_seed_load());
}

// Bitwise hash of an IEEE-754 binary128, taken BY POINTER (16 bytes at `value_ptr`):
// a 128-bit parameter goes indirectly under Win64 and in register pairs elsewhere,
// so by-value made the JIT reject the module. Both halves mix; -0.0 folds to +0.0.
int64_t __cajeta_hash_float128(const void* value_ptr) {
    unsigned char bits[16];
    memcpy(bits, value_ptr, sizeof(bits));
    int signOnly = (bits[15] == 0x80);
    for (int i = 0; i < 15 && signOnly; i++) if (bits[i]) signOnly = 0;
    if (signOnly) bits[15] = 0;            // -0.0 -> +0.0
    uint64_t lo, hi;
    memcpy(&lo, bits, 8);
    memcpy(&hi, bits + 8, 8);
    uint64_t h = splitmix64_finalize(lo ^ __cajeta_hash_seed_load());
    h = splitmix64_finalize(h ^ hi);
    return (int64_t) h;
}

int64_t __cajeta_hash_boolean(int8_t value) {
    return (int64_t) splitmix64_finalize(
        (value ? 1ULL : 0ULL) ^ __cajeta_hash_seed_load());
}

// Guid hash — both halves through SplitMix; Guid.equals() is the exact compare.
int64_t __cajeta_hash_guid(int64_t hi, int64_t lo) {
    uint64_t h = splitmix64_finalize((uint64_t) hi ^ __cajeta_hash_seed_load());
    h = splitmix64_finalize(h ^ (uint64_t) lo);
    return (int64_t) h;
}

// Fill `n` bytes with entropy — BCryptGenRandom or /dev/urandom, else rand().
static void cajeta_fill_entropy(unsigned char* b, int n) {
#if defined(_WIN32)
    if (BCryptGenRandom(NULL, (PUCHAR) b, (ULONG) n,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 /* STATUS_SUCCESS */) {
        return;
    }
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        int got = 0;
        while (got < n) {
            ssize_t r = read(fd, b + got, (size_t) (n - got));
            if (r <= 0) break;
            got += (int) r;
        }
        close(fd);
        if (got == n) return;
    }
#endif
    for (int i = 0; i < n; i++) b[i] = (unsigned char) (rand() & 0xFF);
}

// cajeta.lang.Guid.random() — an RFC 4122 version-4 UUID into a cajeta int64[2]
// ({ i64 count; i64 hi; i64 lo }), version nibble and variant bits forced.
void __cajeta_guid_random_fill(void* out) {
    if (!out) return;
    unsigned char b[16];
    cajeta_fill_entropy(b, 16);
    b[6] = (unsigned char) ((b[6] & 0x0F) | 0x40);   // version 4
    b[8] = (unsigned char) ((b[8] & 0x3F) | 0x80);   // variant 10xx
    uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 8; i++)  hi = (hi << 8) | b[i];
    for (int i = 8; i < 16; i++) lo = (lo << 8) | b[i];
    int64_t* o = (int64_t*) out;
    o[1] = (int64_t) hi;   // o[0] is the array's count header
    o[2] = (int64_t) lo;
}

// Pointer-identity hash, on the same mixer as the primitive variants.
int64_t __cajeta_hash_identity(void* p) {
    return (int64_t) splitmix64_finalize(
        (uint64_t)(uintptr_t) p ^ __cajeta_hash_seed_load());
}

// Combine two 64-bit hash values — Boost's hash_combine over the SplitMix mixer.
int64_t __cajeta_hash_combine(int64_t a, int64_t b) {
    uint64_t h = (uint64_t) a;
    h ^= (uint64_t) b + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return (int64_t) splitmix64_finalize(h);
}

// XXH3-64 over a byte buffer, seeded per process through XXH3's seed parameter.
int64_t __cajeta_hash_bytes(const uint8_t* data, int64_t len) {
    if (len < 0) len = 0;
    return (int64_t) XXH3_64bits_withSeed(
        data, (size_t) len, __cajeta_hash_seed_load());
}

// Same algorithm with a caller-supplied seed, for replay and deterministic tests.
int64_t __cajeta_hash_bytes_seeded(const uint8_t* data, int64_t len, int64_t seed) {
    if (len < 0) len = 0;
    return (int64_t) XXH3_64bits_withSeed(
        data, (size_t) len, (uint64_t) seed);
}

// --- cajeta.hash.MD5 --------------------------------------------------------
// RFC 1321 MD5 — broken, kept for ETag / Content-MD5 / cache keys. 16-byte digest.

struct cajeta_md5_state {
    uint32_t s[4];          // A, B, C, D
    uint64_t bits;          // total bytes hashed * 8
    uint8_t  buf[64];       // partial-block buffer
    int32_t  buf_len;
};

// K and S are baked into the unrolled md5_transform, so no runtime tables.

static inline uint32_t md5_rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32u - n));
}

// Fully-unrolled MD5 transform: index, rotate and constant are baked in per round,
// so no data-dependent branch or indexed K/S survives the inner loop.
static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    // memcpy into uint32 lowers to unaligned LE word loads; BE would need a bswap.
    uint32_t M[16];
    memcpy(M, block, 64);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    #define MD5_F(x,y,z) ((z) ^ ((x) & ((y) ^ (z))))
    #define MD5_G(x,y,z) ((y) ^ ((z) & ((x) ^ (y))))
    #define MD5_H(x,y,z) ((x) ^ (y) ^ (z))
    #define MD5_I(x,y,z) ((y) ^ ((x) | ~(z)))
    #define MD5_STEP(fn,a,b,c,d,m,k,s) (a) = (b) + md5_rotl((a) + fn(b,c,d) + (m) + (k), s)
    MD5_STEP(MD5_F,a,b,c,d,M[0],0xd76aa478,7);  MD5_STEP(MD5_F,d,a,b,c,M[1],0xe8c7b756,12);
    MD5_STEP(MD5_F,c,d,a,b,M[2],0x242070db,17); MD5_STEP(MD5_F,b,c,d,a,M[3],0xc1bdceee,22);
    MD5_STEP(MD5_F,a,b,c,d,M[4],0xf57c0faf,7);  MD5_STEP(MD5_F,d,a,b,c,M[5],0x4787c62a,12);
    MD5_STEP(MD5_F,c,d,a,b,M[6],0xa8304613,17); MD5_STEP(MD5_F,b,c,d,a,M[7],0xfd469501,22);
    MD5_STEP(MD5_F,a,b,c,d,M[8],0x698098d8,7);  MD5_STEP(MD5_F,d,a,b,c,M[9],0x8b44f7af,12);
    MD5_STEP(MD5_F,c,d,a,b,M[10],0xffff5bb1,17);MD5_STEP(MD5_F,b,c,d,a,M[11],0x895cd7be,22);
    MD5_STEP(MD5_F,a,b,c,d,M[12],0x6b901122,7); MD5_STEP(MD5_F,d,a,b,c,M[13],0xfd987193,12);
    MD5_STEP(MD5_F,c,d,a,b,M[14],0xa679438e,17);MD5_STEP(MD5_F,b,c,d,a,M[15],0x49b40821,22);
    MD5_STEP(MD5_G,a,b,c,d,M[1],0xf61e2562,5);  MD5_STEP(MD5_G,d,a,b,c,M[6],0xc040b340,9);
    MD5_STEP(MD5_G,c,d,a,b,M[11],0x265e5a51,14);MD5_STEP(MD5_G,b,c,d,a,M[0],0xe9b6c7aa,20);
    MD5_STEP(MD5_G,a,b,c,d,M[5],0xd62f105d,5);  MD5_STEP(MD5_G,d,a,b,c,M[10],0x02441453,9);
    MD5_STEP(MD5_G,c,d,a,b,M[15],0xd8a1e681,14);MD5_STEP(MD5_G,b,c,d,a,M[4],0xe7d3fbc8,20);
    MD5_STEP(MD5_G,a,b,c,d,M[9],0x21e1cde6,5);  MD5_STEP(MD5_G,d,a,b,c,M[14],0xc33707d6,9);
    MD5_STEP(MD5_G,c,d,a,b,M[3],0xf4d50d87,14); MD5_STEP(MD5_G,b,c,d,a,M[8],0x455a14ed,20);
    MD5_STEP(MD5_G,a,b,c,d,M[13],0xa9e3e905,5); MD5_STEP(MD5_G,d,a,b,c,M[2],0xfcefa3f8,9);
    MD5_STEP(MD5_G,c,d,a,b,M[7],0x676f02d9,14); MD5_STEP(MD5_G,b,c,d,a,M[12],0x8d2a4c8a,20);
    MD5_STEP(MD5_H,a,b,c,d,M[5],0xfffa3942,4);  MD5_STEP(MD5_H,d,a,b,c,M[8],0x8771f681,11);
    MD5_STEP(MD5_H,c,d,a,b,M[11],0x6d9d6122,16);MD5_STEP(MD5_H,b,c,d,a,M[14],0xfde5380c,23);
    MD5_STEP(MD5_H,a,b,c,d,M[1],0xa4beea44,4);  MD5_STEP(MD5_H,d,a,b,c,M[4],0x4bdecfa9,11);
    MD5_STEP(MD5_H,c,d,a,b,M[7],0xf6bb4b60,16); MD5_STEP(MD5_H,b,c,d,a,M[10],0xbebfbc70,23);
    MD5_STEP(MD5_H,a,b,c,d,M[13],0x289b7ec6,4); MD5_STEP(MD5_H,d,a,b,c,M[0],0xeaa127fa,11);
    MD5_STEP(MD5_H,c,d,a,b,M[3],0xd4ef3085,16); MD5_STEP(MD5_H,b,c,d,a,M[6],0x04881d05,23);
    MD5_STEP(MD5_H,a,b,c,d,M[9],0xd9d4d039,4);  MD5_STEP(MD5_H,d,a,b,c,M[12],0xe6db99e5,11);
    MD5_STEP(MD5_H,c,d,a,b,M[15],0x1fa27cf8,16);MD5_STEP(MD5_H,b,c,d,a,M[2],0xc4ac5665,23);
    MD5_STEP(MD5_I,a,b,c,d,M[0],0xf4292244,6);  MD5_STEP(MD5_I,d,a,b,c,M[7],0x432aff97,10);
    MD5_STEP(MD5_I,c,d,a,b,M[14],0xab9423a7,15);MD5_STEP(MD5_I,b,c,d,a,M[5],0xfc93a039,21);
    MD5_STEP(MD5_I,a,b,c,d,M[12],0x655b59c3,6); MD5_STEP(MD5_I,d,a,b,c,M[3],0x8f0ccc92,10);
    MD5_STEP(MD5_I,c,d,a,b,M[10],0xffeff47d,15);MD5_STEP(MD5_I,b,c,d,a,M[1],0x85845dd1,21);
    MD5_STEP(MD5_I,a,b,c,d,M[8],0x6fa87e4f,6);  MD5_STEP(MD5_I,d,a,b,c,M[15],0xfe2ce6e0,10);
    MD5_STEP(MD5_I,c,d,a,b,M[6],0xa3014314,15); MD5_STEP(MD5_I,b,c,d,a,M[13],0x4e0811a1,21);
    MD5_STEP(MD5_I,a,b,c,d,M[4],0xf7537e82,6);  MD5_STEP(MD5_I,d,a,b,c,M[11],0xbd3af235,10);
    MD5_STEP(MD5_I,c,d,a,b,M[2],0x2ad7d2bb,15); MD5_STEP(MD5_I,b,c,d,a,M[9],0xeb86d391,21);
    #undef MD5_F
    #undef MD5_G
    #undef MD5_H
    #undef MD5_I
    #undef MD5_STEP
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(struct cajeta_md5_state* s) {
    s->s[0] = 0x67452301;
    s->s[1] = 0xefcdab89;
    s->s[2] = 0x98badcfe;
    s->s[3] = 0x10325476;
    s->bits = 0;
    s->buf_len = 0;
}

static void md5_update(struct cajeta_md5_state* s,
                       const uint8_t* data, size_t len) {
    s->bits += (uint64_t) len * 8u;
    if (s->buf_len > 0) {
        size_t need = (size_t) (64 - s->buf_len);
        size_t take = need < len ? need : len;
        memcpy(s->buf + s->buf_len, data, take);
        s->buf_len += (int32_t) take;
        data += take; len -= take;
        if (s->buf_len == 64) { md5_transform(s->s, s->buf); s->buf_len = 0; }
    }
    // Whole blocks hash straight from the caller's buffer: no per-block memcpy.
    while (len >= 64) {
        md5_transform(s->s, data);
        data += 64; len -= 64;
    }
    // Stash the sub-block tail for next time.
    if (len > 0) {
        memcpy(s->buf, data, len);
        s->buf_len = (int32_t) len;
    }
}

static void md5_finalize(struct cajeta_md5_state* s, uint8_t out[16]) {
    // Append 0x80, pad to 56 mod 64, append the 8-byte LE bit count, transform.
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) {
        memset(s->buf + s->buf_len, 0, (size_t)(64 - s->buf_len));
        md5_transform(s->s, s->buf);
        s->buf_len = 0;
    }
    memset(s->buf + s->buf_len, 0, (size_t)(56 - s->buf_len));
    for (int i = 0; i < 8; i++) {
        s->buf[56 + i] = (uint8_t)(s->bits >> (i * 8));
    }
    md5_transform(s->s, s->buf);
    for (int i = 0; i < 4; i++) {
        out[i*4 + 0] = (uint8_t)(s->s[i] >> 0);
        out[i*4 + 1] = (uint8_t)(s->s[i] >> 8);
        out[i*4 + 2] = (uint8_t)(s->s[i] >> 16);
        out[i*4 + 3] = (uint8_t)(s->s[i] >> 24);
    }
}

// --- MD5 C ABI bridges -----------------------------------------------------
// Streaming state, opaque to cajeta; allocator and finalizer pair as the class does.

void* __cajeta_md5_alloc(void) {
    struct cajeta_md5_state* s = (struct cajeta_md5_state*) malloc(sizeof *s);
    if (!s) return NULL;
    md5_init(s);
    return s;
}

void __cajeta_md5_free(void* state) {
    if (state) free(state);
}

void __cajeta_md5_reset(void* state) {
    if (state) md5_init((struct cajeta_md5_state*) state);
}

// `data_hdr` is a cajeta int8[] header, so bytes start at offset 8; `len` is explicit.
void __cajeta_md5_update(void* state, const void* data_hdr, int64_t len) {
    if (!state || !data_hdr || len <= 0) return;
    const uint8_t* data = ((const uint8_t*) data_hdr) + 8;
    md5_update((struct cajeta_md5_state*) state, data, (size_t) len);
}

// out_hdr is a cajeta int8[16] header; 16 bytes are written at offset 8.
void __cajeta_md5_finalize_into(void* state, void* out_hdr) {
    if (!state || !out_hdr) return;
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    md5_finalize((struct cajeta_md5_state*) state, out);
}

// Width-named folders writing little-endian bytes; Hasher's contract pins width.
void __cajeta_md5_write_i8 (void* state, int8_t  v) {
    if (state) md5_update((struct cajeta_md5_state*) state, (const uint8_t*) &v, 1);
}
void __cajeta_md5_write_i16(void* state, int16_t v) {
    if (!state) return;
    uint8_t b[2] = { (uint8_t)(v), (uint8_t)(v >> 8) };
    md5_update((struct cajeta_md5_state*) state, b, 2);
}
void __cajeta_md5_write_i32(void* state, int32_t v) {
    if (!state) return;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
    md5_update((struct cajeta_md5_state*) state, b, 4);
}
void __cajeta_md5_write_i64(void* state, int64_t v) {
    if (!state) return;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    md5_update((struct cajeta_md5_state*) state, b, 8);
}
void __cajeta_md5_write_f32(void* state, float v) {
    if (!state) return;
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_md5_write_i32(state, (int32_t) bits);
}
void __cajeta_md5_write_f64(void* state, double v) {
    if (!state) return;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_md5_write_i64(state, (int64_t) bits);
}
void __cajeta_md5_write_bool(void* state, int8_t v) {
    __cajeta_md5_write_i8(state, v ? 1 : 0);
}

// finish() projection: the first 8 digest bytes as a little-endian int64. This
// FINALIZES the state, so a second finish() is garbage — finish is terminal.
int64_t __cajeta_md5_finish_int64(void* state) {
    if (!state) return 0;
    uint8_t digest[16];
    md5_finalize((struct cajeta_md5_state*) state, digest);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t) digest[i]) << (i * 8);
    }
    return (int64_t) v;
}

// One-shot variants filling the caller's buffer at `out_hdr + 8` (no int8[] return).
void __cajeta_md5_oneshot_into(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    struct cajeta_md5_state s;
    md5_init(&s);
    if (data_hdr && len > 0) {
        md5_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    md5_finalize(&s, ((uint8_t*) out_hdr) + 8);
}

// Lowercase hex digest into a caller-supplied int8[32] buffer.
void __cajeta_md5_oneshot_hex_into(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    struct cajeta_md5_state s;
    md5_init(&s);
    if (data_hdr && len > 0) {
        md5_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    uint8_t digest[16];
    md5_finalize(&s, digest);
    static const char HEX[16] = "0123456789abcdef";
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    for (int i = 0; i < 16; i++) {
        out[i*2 + 0] = (uint8_t) HEX[(digest[i] >> 4) & 0xF];
        out[i*2 + 1] = (uint8_t) HEX[digest[i] & 0xF];
    }
}

// --- cajeta.hash.SHA-256 (NET-11.1, FIPS 180-4) ----------------------------
// #included, not compiled: only cajeta_runtime.c goes to bitcode, so a sibling
// .c must be textual to be embedded into user modules.
#include "cajeta_sha256.c"
#include "cajeta_blake3.c"

// --- cajeta.hash.SipHash (SipHash-2-4) -------------------------------------
// SipHash-2-4 over arbitrary bytes with a 128-bit key: hash-flooding resistant, for
// untrusted keys. The in-process default stays XXH3, an order of magnitude faster.

struct cajeta_siphash_state {
    uint64_t v0, v1, v2, v3;     // working state
    uint8_t  buf[8];             // partial-word buffer
    int32_t  buf_len;
    uint64_t total_bytes;
};

static inline uint64_t sip_rotl(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

static inline void sip_round(uint64_t* v0, uint64_t* v1,
                             uint64_t* v2, uint64_t* v3) {
    *v0 += *v1; *v1 = sip_rotl(*v1, 13); *v1 ^= *v0; *v0 = sip_rotl(*v0, 32);
    *v2 += *v3; *v3 = sip_rotl(*v3, 16); *v3 ^= *v2;
    *v0 += *v3; *v3 = sip_rotl(*v3, 21); *v3 ^= *v0;
    *v2 += *v1; *v1 = sip_rotl(*v1, 17); *v1 ^= *v2; *v2 = sip_rotl(*v2, 32);
}

static inline uint64_t sip_load_le64(const uint8_t* p) {
    return ((uint64_t) p[0])
         | ((uint64_t) p[1] << 8)
         | ((uint64_t) p[2] << 16)
         | ((uint64_t) p[3] << 24)
         | ((uint64_t) p[4] << 32)
         | ((uint64_t) p[5] << 40)
         | ((uint64_t) p[6] << 48)
         | ((uint64_t) p[7] << 56);
}

static void siphash_init(struct cajeta_siphash_state* s,
                         uint64_t k0, uint64_t k1) {
    s->v0 = k0 ^ 0x736f6d6570736575ULL;
    s->v1 = k1 ^ 0x646f72616e646f6dULL;
    s->v2 = k0 ^ 0x6c7967656e657261ULL;
    s->v3 = k1 ^ 0x7465646279746573ULL;
    s->buf_len = 0;
    s->total_bytes = 0;
}

static void siphash_absorb_block(struct cajeta_siphash_state* s,
                                 const uint8_t* block) {
    uint64_t m = sip_load_le64(block);
    s->v3 ^= m;
    sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    s->v0 ^= m;
}

static void siphash_update(struct cajeta_siphash_state* s,
                           const uint8_t* data, size_t len) {
    s->total_bytes += len;
    while (s->buf_len > 0 && len > 0) {
        size_t to_copy = (size_t)(8 - s->buf_len);
        if (to_copy > len) to_copy = len;
        memcpy(s->buf + s->buf_len, data, to_copy);
        s->buf_len += (int32_t) to_copy;
        data += to_copy; len -= to_copy;
        if (s->buf_len == 8) {
            siphash_absorb_block(s, s->buf);
            s->buf_len = 0;
        }
    }
    while (len >= 8) {
        siphash_absorb_block(s, data);
        data += 8; len -= 8;
    }
    if (len > 0) {
        memcpy(s->buf, data, len);
        s->buf_len = (int32_t) len;
    }
}

static uint64_t siphash_finalize(struct cajeta_siphash_state* s) {
    // Final block: remaining bytes + length-modulo-256 in top byte.
    uint8_t last[8] = {0};
    memcpy(last, s->buf, (size_t) s->buf_len);
    last[7] = (uint8_t)(s->total_bytes & 0xFF);
    uint64_t m = sip_load_le64(last);
    s->v3 ^= m;
    sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    s->v0 ^= m;
    // Finalization rounds (4 for SipHash-2-4).
    s->v2 ^= 0xFF;
    for (int i = 0; i < 4; i++) {
        sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    }
    return s->v0 ^ s->v1 ^ s->v2 ^ s->v3;
}

// --- SipHash C ABI bridges -------------------------------------------------

void* __cajeta_siphash_alloc(int64_t k0, int64_t k1) {
    struct cajeta_siphash_state* s = (struct cajeta_siphash_state*) malloc(sizeof *s);
    if (!s) return NULL;
    siphash_init(s, (uint64_t) k0, (uint64_t) k1);
    return s;
}

void __cajeta_siphash_free(void* state) {
    if (state) free(state);
}

void __cajeta_siphash_reset(void* state, int64_t k0, int64_t k1) {
    if (state) siphash_init((struct cajeta_siphash_state*) state,
                            (uint64_t) k0, (uint64_t) k1);
}

void __cajeta_siphash_update(void* state, const void* data_hdr, int64_t len) {
    if (!state || !data_hdr || len <= 0) return;
    siphash_update((struct cajeta_siphash_state*) state,
                   ((const uint8_t*) data_hdr) + 8, (size_t) len);
}

// finish() projection, and the natural digest: SipHash is 64-bit throughout.
int64_t __cajeta_siphash_finish(void* state) {
    if (!state) return 0;
    return (int64_t) siphash_finalize((struct cajeta_siphash_state*) state);
}

int64_t __cajeta_siphash_oneshot(const void* data_hdr, int64_t len,
                                 int64_t k0, int64_t k1) {
    struct cajeta_siphash_state s;
    siphash_init(&s, (uint64_t) k0, (uint64_t) k1);
    if (data_hdr && len > 0) {
        siphash_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    return (int64_t) siphash_finalize(&s);
}

// Width-named SipHash folders — same shape as MD5's.
void __cajeta_siphash_write_i8(void* state, int8_t v) {
    if (state) siphash_update((struct cajeta_siphash_state*) state,
                              (const uint8_t*) &v, 1);
}
void __cajeta_siphash_write_i16(void* state, int16_t v) {
    if (!state) return;
    uint8_t b[2] = { (uint8_t) v, (uint8_t)(v >> 8) };
    siphash_update((struct cajeta_siphash_state*) state, b, 2);
}
void __cajeta_siphash_write_i32(void* state, int32_t v) {
    if (!state) return;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
    siphash_update((struct cajeta_siphash_state*) state, b, 4);
}
void __cajeta_siphash_write_i64(void* state, int64_t v) {
    if (!state) return;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    siphash_update((struct cajeta_siphash_state*) state, b, 8);
}
void __cajeta_siphash_write_f32(void* state, float v) {
    if (!state) return;
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_siphash_write_i32(state, (int32_t) bits);
}
void __cajeta_siphash_write_f64(void* state, double v) {
    if (!state) return;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_siphash_write_i64(state, (int64_t) bits);
}

// --- cajeta.hash.XXHash3 (XXH3-64) ----------------------------------------
// alloc/update/digest: the XXHash3 class's opaque-pointer streaming surface.

void* __cajeta_xxh3_alloc(int64_t seed) {
    XXH3_state_t* s = XXH3_createState();
    if (!s) return NULL;
    XXH3_64bits_reset_withSeed(s, (XXH64_hash_t) seed);
    return s;
}

void __cajeta_xxh3_free(void* state) {
    if (state) XXH3_freeState((XXH3_state_t*) state);
}

void __cajeta_xxh3_reset(void* state, int64_t seed) {
    if (state) XXH3_64bits_reset_withSeed(
        (XXH3_state_t*) state, (XXH64_hash_t) seed);
}

void __cajeta_xxh3_update(void* state, const void* data_hdr, int64_t len) {
    if (!state || !data_hdr || len <= 0) return;
    XXH3_64bits_update((XXH3_state_t*) state,
                       ((const uint8_t*) data_hdr) + 8, (size_t) len);
}

int64_t __cajeta_xxh3_finish(void* state) {
    if (!state) return 0;
    return (int64_t) XXH3_64bits_digest((const XXH3_state_t*) state);
}

int64_t __cajeta_xxh3_oneshot(const void* data_hdr, int64_t len, int64_t seed) {
    if (!data_hdr || len <= 0) return 0;
    return (int64_t) XXH3_64bits_withSeed(
        ((const uint8_t*) data_hdr) + 8, (size_t) len, (uint64_t) seed);
}

// One-shot XXH3-128: low64 then high64 (native LE) into the 16-byte payload of
// out_hdr (data at +8). The oracle for the Cajeta SIMD path, and its short path.
void __cajeta_xxh3_128_oneshot(const void* data_hdr, int64_t len, int64_t seed,
                               void* out_hdr) {
    if (!out_hdr) return;
    const uint8_t* in = (len > 0 && data_hdr) ? ((const uint8_t*) data_hdr) + 8
                                              : (const uint8_t*) "";
    XXH128_hash_t h = XXH3_128bits_withSeed(in, (size_t)(len > 0 ? len : 0),
                                            (uint64_t) seed);
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    memcpy(out, &h.low64, 8);
    memcpy(out + 8, &h.high64, 8);
}

// An XXH3-128 pair as the 32-char canonical hex digest (big-endian high64, then low64).
void __cajeta_xxh3_128_hex(int64_t low, int64_t high, void* out_hdr) {
    if (!out_hdr) return;
    static const char H[16] = {'0','1','2','3','4','5','6','7',
                               '8','9','a','b','c','d','e','f'};
    uint8_t* o = ((uint8_t*) out_hdr) + 8;
    uint64_t hi = (uint64_t) high, lo = (uint64_t) low;
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(hi >> ((7 - i) * 8));
        o[i * 2] = H[b >> 4]; o[i * 2 + 1] = H[b & 0xF];
    }
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(lo >> ((7 - i) * 8));
        o[16 + i * 2] = H[b >> 4]; o[16 + i * 2 + 1] = H[b & 0xF];
    }
}

// Width-named folders. Same approach as MD5 / SipHash.
void __cajeta_xxh3_write_i8(void* state, int8_t v) {
    if (state) XXH3_64bits_update((XXH3_state_t*) state, &v, 1);
}
void __cajeta_xxh3_write_i16(void* state, int16_t v) {
    if (!state) return;
    uint8_t b[2] = { (uint8_t) v, (uint8_t)(v >> 8) };
    XXH3_64bits_update((XXH3_state_t*) state, b, 2);
}
void __cajeta_xxh3_write_i32(void* state, int32_t v) {
    if (!state) return;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
    XXH3_64bits_update((XXH3_state_t*) state, b, 4);
}
void __cajeta_xxh3_write_i64(void* state, int64_t v) {
    if (!state) return;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    XXH3_64bits_update((XXH3_state_t*) state, b, 8);
}
void __cajeta_xxh3_write_f32(void* state, float v) {
    if (!state) return;
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_xxh3_write_i32(state, (int32_t) bits);
}
void __cajeta_xxh3_write_f64(void* state, double v) {
    if (!state) return;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_xxh3_write_i64(state, (int64_t) bits);
}

// --- cajeta.lang.Object root methods ----------------------------------------
