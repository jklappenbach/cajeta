// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// --- general-purpose hashing (cajeta.hash backend) --------------------------
// SplitMix64 for primitive values, XXH3-64 for bytes, both XOR'd with a per-process seed.

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
