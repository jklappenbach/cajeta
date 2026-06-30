// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- general-purpose hashing (cajeta.hash backend) --------------------------
// Implements the runtime hash primitives the language uses for Object.hash()
// and the cajeta.hash.* stdlib classes (see StandardLibrary.md §cajeta.hash
// and CajetaReflect.md "Performance"). Two algorithms cover the surface:
//
//   * SplitMix64 finalizer for primitive value hashing — int64.hash(),
//     int32.hash(), float64.hash(), float32.hash(), boolean.hash(),
//     pointer identity. Three multiplications + three XORs; well-tested
//     mixer (Java's SplittableRandom, Rust hashers, etc.). The per-process
//     seed is XOR'd in before mixing so two runs of the same program
//     produce different hash values (hash-flooding defense — attackers
//     can't predict bucket placement).
//
//   * XXH3-64 (scalar) for arbitrary byte buffers — backing for
//     cajeta.hash.XXHash3 and DefaultHasher. Multi-GB/s on modern CPUs.
//     Lands in a follow-up commit; this one ships the primitive-hash +
//     seed infrastructure first because HashMap<int64, V> and similar
//     primitive-keyed maps don't need it.
//
// The seed initializes once per process from /dev/urandom via a
// constructor function that fires before main(). Falls back to
// wall-clock + pid mixed through SplitMix64 if /dev/urandom isn't
// readable (sandboxes, embedded targets).

#include <fcntl.h>
#include <unistd.h>
#include <time.h>

// XXH_INLINE_ALL: the xxhash header ships in two modes — declare-only
// (link against libxxhash) and inline-all (full implementation in this
// translation unit). We pick inline-all so the runtime's bitcode + native
// build both carry the implementation; no separate libxxhash linkage step
// on the JIT or AOT side. -O2 dead-code-strips the unused XXH32/XXH64/
// XXH128 paths so binary growth is bounded to what we actually call.
#define XXH_INLINE_ALL
#include <xxhash.h>

static uint64_t __cajeta_hash_seed_value = 0;

__attribute__((constructor))
static void __cajeta_hash_seed_init(void) {
    uint64_t s = 0;
#if defined(_WIN32)
    // Windows: BCryptGenRandom (CNG) is the modern equivalent of
    // /dev/urandom — cryptographically strong, no /dev needed.
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG saves us providing an algorithm
    // provider handle. Available since Vista.
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
    // Fallback: wall clock + pid mixed through SplitMix64. Lower-entropy
    // than /dev/urandom but still per-process-distinct and stable for
    // the lifetime of the process.
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

// Lazy seed accessor. The constructor function above pre-initializes
// the seed at process startup — but only in the native binary build
// of this file. JIT-loaded bitcode copies have a separate static
// __cajeta_hash_seed_value that the JIT doesn't auto-initialize (no
// .init_array invocation at module-load). Checking-and-initializing
// on first call keeps both paths correct. After the first call the
// branch is predicted-not-taken and folds away in hot code.
static inline uint64_t __cajeta_hash_seed_load(void) {
    uint64_t s = __cajeta_hash_seed_value;
    if (__builtin_expect(s == 0, 0)) {
        __cajeta_hash_seed_init();
        s = __cajeta_hash_seed_value;
    }
    return s;
}

// Exposed to user code as cajeta.hash.Hash.processSeed() — useful when
// caller-side hashing needs to align with the synthesized Object.hash()
// values (e.g. external hash table snapshot replay).
int64_t __cajeta_hash_seed(void) {
    return (int64_t) __cajeta_hash_seed_load();
}

// SplitMix64 finalizer — the mixer behind every primitive hash variant.
// Three multiplications + three XOR-shifts; passes SMHasher avalanche
// + bias + collision tests on its own.
static inline uint64_t splitmix64_finalize(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Cheaper integer mix than splitmix64_finalize (1 mul + 1 xorshift vs 2 mul +
// 3 xorshift). Still a BIJECTION on uint64 (odd-constant multiply and xorshift
// are both invertible), so distinct keys always hash distinctly. The `>> 29`
// fold pushes the well-mixed high bits down into the low bits the SwissTable
// uses for its bucket index. Hot path for every int-keyed HashMap/HashSet.
static inline uint64_t fast_int_mix(uint64_t x) {
    x *= 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29;
    return x;
}

int64_t __cajeta_hash_int64(int64_t value) {
    return (int64_t) fast_int_mix((uint64_t) value ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_int32(int32_t value) {
    // Sign-extend so all-ones int32 doesn't hash like ~0 int64 just by
    // happening to share the low bits.
    return (int64_t) fast_int_mix(
        (uint64_t) (int64_t) value ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_float64(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    // Canonicalize -0 to +0 — IEEE 754 says +0 == -0, so they must hash
    // identically. NaN ordering is unspecified by the standard; we hash
    // each distinct NaN bit pattern to a distinct value, which is what
    // serializers / HashMap callers usually want.
    if (bits == 0x8000000000000000ULL) bits = 0;
    return (int64_t) splitmix64_finalize(bits ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_float32(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if (bits == 0x80000000U) bits = 0;
    return (int64_t) splitmix64_finalize((uint64_t) bits ^ __cajeta_hash_seed_load());
}

// Bitwise hash of an IEEE-754 binary128 (LLVM fp128), plan W3. Takes the raw
// 128 bits BY POINTER (16 bytes at `value_ptr`), not by value. An earlier
// version took `__uint128_t` by value, but that param's ABI is NOT uniform
// across our targets: x86-64 SysV / AArch64 pass i128 in register pairs, but
// the Win64 (mingw) ABI passes a 128-bit integer INDIRECTLY — clang lowers the
// param to `i64(ptr dead_on_return)`. The compiler emitted the call as
// `i64(i128)` (matching Linux/macOS), so on mingw the JIT-verify rejected the
// embedded module ("Call parameter type does not match function signature") and
// EVERY test failed. Passing by pointer makes the ABI `i64(ptr)` on all three
// targets. Method::emitNativeForwardingBody stores the fp128 to a stack slot
// and passes its address (a bitcast/store, no soft-float), so Float128.hashBits
// lowers to a matching `call i64 @__cajeta_hash_float128(ptr ...)`. (`__float128`
// isn't spellable on aarch64 anyway; we never name the C float type here.)
// float16/bfloat16 hash by widening to float64 (lossless, injective) and
// reusing __cajeta_hash_float64, but float128 → float64 is *lossy*, so distinct
// float128 values could collide and wrongly compare equal (Object.operator== is
// hash-equality). Hash the full 128 bits instead: canonicalize -0.0 to +0.0
// (IEEE says +0 == -0) and mix both 64-bit halves through the shared SplitMix
// finalizer. x86-64 is little-endian, so the sign bit is the MSB of the high
// half (bits[15] & 0x80); -0.0 is sign-only with an all-zero significand/exp.
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

// cajeta.lang.Guid hash — mixes both 64-bit halves of the 128-bit value through
// the shared SplitMix finalizer (same construction as __cajeta_hash_float128).
// 128 bits of identity can't inject into a 64-bit hash, so Object.operator==
// (hash equality) carries the usual ~2^-64 collision caveat; Guid.equals() is
// the exact 128-bit comparison.
int64_t __cajeta_hash_guid(int64_t hi, int64_t lo) {
    uint64_t h = splitmix64_finalize((uint64_t) hi ^ __cajeta_hash_seed_load());
    h = splitmix64_finalize(h ^ (uint64_t) lo);
    return (int64_t) h;
}

// Fill `n` bytes with cryptographic entropy: BCryptGenRandom (Windows) /
// /dev/urandom (POSIX), with a rand() fallback only if the OS CSPRNG is
// unavailable. Mirrors the per-process hash-seed init above so Guid.random()
// is strong on every platform, not just where /dev/urandom exists.
static void cajeta_fill_entropy(unsigned char* b, int n) {
#if defined(_WIN32)
    // <bcrypt.h> is included at file scope (after windows.h); see note there.
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

// cajeta.lang.Guid.random() — generate a RFC 4122 version-4 (random) UUID.
// `out` is a cajeta int64[2] ({ i64 count; i64 hi; i64 lo }); we fill the two
// element slots with the big-endian-packed high/low 64 bits. Version nibble (4)
// and variant bits (10xx) are forced per the spec.
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

// Pointer-identity hash. Used by IdentityHashMap, observer registries,
// weak-ref tables. Same mixer as the primitive variants so the
// distribution properties match.
int64_t __cajeta_hash_identity(void* p) {
    return (int64_t) splitmix64_finalize(
        (uint64_t)(uintptr_t) p ^ __cajeta_hash_seed_load());
}

// Combine two 64-bit hash values into one. Boost's hash_combine pattern
// adapted with the SplitMix mixer at the end. Used by manual hash()
// overrides that thread multiple field hashes together.
int64_t __cajeta_hash_combine(int64_t a, int64_t b) {
    uint64_t h = (uint64_t) a;
    h ^= (uint64_t) b + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return (int64_t) splitmix64_finalize(h);
}

// XXH3-64 over an arbitrary byte buffer. Backs cajeta.hash.XXHash3 and
// String.hash() (where String is a UTF-8 sequence). Per-process seed
// is mixed via XXH3's seed parameter — same hash-flooding defense
// the primitive variants get. Multi-GB/s on modern CPUs; small-input
// path (the typical field-hashing case) is a handful of cycles.
int64_t __cajeta_hash_bytes(const uint8_t* data, int64_t len) {
    if (len < 0) len = 0;
    return (int64_t) XXH3_64bits_withSeed(
        data, (size_t) len, __cajeta_hash_seed_load());
}

// Same algorithm with caller-supplied seed. For cases where the seed
// is part of the input (snapshot replay, cross-process hash table
// rendezvous, deterministic-test contexts).
int64_t __cajeta_hash_bytes_seeded(const uint8_t* data, int64_t len, int64_t seed) {
    if (len < 0) len = 0;
    return (int64_t) XXH3_64bits_withSeed(
        data, (size_t) len, (uint64_t) seed);
}

// --- cajeta.hash.MD5 --------------------------------------------------------
// RFC 1321 MD5. Cryptographically broken — surfaced here for HTTP ETag,
// S3 Content-MD5, asset fingerprinting, cache-key derivation, database
// row fingerprinting. The full digest is 16 bytes; the streaming
// Hasher.finish() projection returns the first 8 bytes as little-endian
// int64 (matching how SipHash / XXH3 fold to int64). Callers that need
// the full 16-byte digest call MD5.hash(...) or MD5.hashHex(...).

struct cajeta_md5_state {
    uint32_t s[4];          // A, B, C, D
    uint64_t bits;          // total bytes hashed * 8
    uint8_t  buf[64];       // partial-block buffer
    int32_t  buf_len;
};

// The per-round constants (K) and rotate amounts (S) are baked directly into
// the unrolled md5_transform below, so no runtime tables are needed.

static inline uint32_t md5_rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32u - n));
}

// Fully-unrolled MD5 transform (OpenSSL/rust-md-5 shape): the per-round message
// index, rotate amount, and constant are baked in, so there's no data-dependent
// branch, no `% 16`, and no array-indexed K/S in the inner loop — the original
// canonical form left ~30% on the table. The round functions use the
// shorter-critical-path boolean identities F = z^(x&(y^z)) and G = y^(z&(x^y)).
// M[] is read little-endian via the byte-OR idiom clang folds to a single load
// on the (LE) targets cajeta builds for.
static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    // The 16 message words are the block read little-endian. memcpy into a
    // uint32 array is the portable idiom clang lowers to direct (unaligned)
    // word loads on the LE targets cajeta builds for — no byte-OR reassembly in
    // the inner path. (On a hypothetical BE target this would need a bswap; all
    // current cajeta targets — x86-64, aarch64, Apple silicon — are LE.)
    uint32_t M[16];
    memcpy(M, block, 64);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    #define MD5_F(x,y,z) ((z) ^ ((x) & ((y) ^ (z))))
    #define MD5_G(x,y,z) ((y) ^ ((z) & ((x) ^ (y))))
    #define MD5_H(x,y,z) ((x) ^ (y) ^ (z))
    #define MD5_I(x,y,z) ((y) ^ ((x) | ~(z)))
    #define MD5_STEP(fn,a,b,c,d,m,k,s) (a) = (b) + md5_rotl((a) + fn(b,c,d) + (m) + (k), s)
    // Round 1
    MD5_STEP(MD5_F,a,b,c,d,M[0],0xd76aa478,7);  MD5_STEP(MD5_F,d,a,b,c,M[1],0xe8c7b756,12);
    MD5_STEP(MD5_F,c,d,a,b,M[2],0x242070db,17); MD5_STEP(MD5_F,b,c,d,a,M[3],0xc1bdceee,22);
    MD5_STEP(MD5_F,a,b,c,d,M[4],0xf57c0faf,7);  MD5_STEP(MD5_F,d,a,b,c,M[5],0x4787c62a,12);
    MD5_STEP(MD5_F,c,d,a,b,M[6],0xa8304613,17); MD5_STEP(MD5_F,b,c,d,a,M[7],0xfd469501,22);
    MD5_STEP(MD5_F,a,b,c,d,M[8],0x698098d8,7);  MD5_STEP(MD5_F,d,a,b,c,M[9],0x8b44f7af,12);
    MD5_STEP(MD5_F,c,d,a,b,M[10],0xffff5bb1,17);MD5_STEP(MD5_F,b,c,d,a,M[11],0x895cd7be,22);
    MD5_STEP(MD5_F,a,b,c,d,M[12],0x6b901122,7); MD5_STEP(MD5_F,d,a,b,c,M[13],0xfd987193,12);
    MD5_STEP(MD5_F,c,d,a,b,M[14],0xa679438e,17);MD5_STEP(MD5_F,b,c,d,a,M[15],0x49b40821,22);
    // Round 2
    MD5_STEP(MD5_G,a,b,c,d,M[1],0xf61e2562,5);  MD5_STEP(MD5_G,d,a,b,c,M[6],0xc040b340,9);
    MD5_STEP(MD5_G,c,d,a,b,M[11],0x265e5a51,14);MD5_STEP(MD5_G,b,c,d,a,M[0],0xe9b6c7aa,20);
    MD5_STEP(MD5_G,a,b,c,d,M[5],0xd62f105d,5);  MD5_STEP(MD5_G,d,a,b,c,M[10],0x02441453,9);
    MD5_STEP(MD5_G,c,d,a,b,M[15],0xd8a1e681,14);MD5_STEP(MD5_G,b,c,d,a,M[4],0xe7d3fbc8,20);
    MD5_STEP(MD5_G,a,b,c,d,M[9],0x21e1cde6,5);  MD5_STEP(MD5_G,d,a,b,c,M[14],0xc33707d6,9);
    MD5_STEP(MD5_G,c,d,a,b,M[3],0xf4d50d87,14); MD5_STEP(MD5_G,b,c,d,a,M[8],0x455a14ed,20);
    MD5_STEP(MD5_G,a,b,c,d,M[13],0xa9e3e905,5); MD5_STEP(MD5_G,d,a,b,c,M[2],0xfcefa3f8,9);
    MD5_STEP(MD5_G,c,d,a,b,M[7],0x676f02d9,14); MD5_STEP(MD5_G,b,c,d,a,M[12],0x8d2a4c8a,20);
    // Round 3
    MD5_STEP(MD5_H,a,b,c,d,M[5],0xfffa3942,4);  MD5_STEP(MD5_H,d,a,b,c,M[8],0x8771f681,11);
    MD5_STEP(MD5_H,c,d,a,b,M[11],0x6d9d6122,16);MD5_STEP(MD5_H,b,c,d,a,M[14],0xfde5380c,23);
    MD5_STEP(MD5_H,a,b,c,d,M[1],0xa4beea44,4);  MD5_STEP(MD5_H,d,a,b,c,M[4],0x4bdecfa9,11);
    MD5_STEP(MD5_H,c,d,a,b,M[7],0xf6bb4b60,16); MD5_STEP(MD5_H,b,c,d,a,M[10],0xbebfbc70,23);
    MD5_STEP(MD5_H,a,b,c,d,M[13],0x289b7ec6,4); MD5_STEP(MD5_H,d,a,b,c,M[0],0xeaa127fa,11);
    MD5_STEP(MD5_H,c,d,a,b,M[3],0xd4ef3085,16); MD5_STEP(MD5_H,b,c,d,a,M[6],0x04881d05,23);
    MD5_STEP(MD5_H,a,b,c,d,M[9],0xd9d4d039,4);  MD5_STEP(MD5_H,d,a,b,c,M[12],0xe6db99e5,11);
    MD5_STEP(MD5_H,c,d,a,b,M[15],0x1fa27cf8,16);MD5_STEP(MD5_H,b,c,d,a,M[2],0xc4ac5665,23);
    // Round 4
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
    // 1. Top off any partial buffer to a full block first.
    if (s->buf_len > 0) {
        size_t need = (size_t) (64 - s->buf_len);
        size_t take = need < len ? need : len;
        memcpy(s->buf + s->buf_len, data, take);
        s->buf_len += (int32_t) take;
        data += take; len -= take;
        if (s->buf_len == 64) { md5_transform(s->s, s->buf); s->buf_len = 0; }
    }
    // 2. Hash whole blocks straight from the caller's buffer — no per-block
    //    64-byte memcpy (the dominant overhead the naive loop paid on bulk input).
    while (len >= 64) {
        md5_transform(s->s, data);
        data += 64; len -= 64;
    }
    // 3. Stash the sub-block tail for next time.
    if (len > 0) {
        memcpy(s->buf, data, len);
        s->buf_len = (int32_t) len;
    }
}

static void md5_finalize(struct cajeta_md5_state* s, uint8_t out[16]) {
    // Append 0x80, pad with zeros to 56 mod 64, append 8-byte
    // little-endian bit count, transform.
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
// Streaming state — opaque to cajeta. Allocator + finalizer match the
// destructor / ctor pattern the cajeta MD5 class uses.

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

// `data_hdr` is a cajeta int8[] header — { i64 count, [N x i8] data }.
// Caller passes the explicit `len` since data.count() isn't always
// known at the call site; the runtime reads bytes from offset 8.
void __cajeta_md5_update(void* state, const void* data_hdr, int64_t len) {
    if (!state || !data_hdr || len <= 0) return;
    const uint8_t* data = ((const uint8_t*) data_hdr) + 8;
    md5_update((struct cajeta_md5_state*) state, data, (size_t) len);
}

// out_hdr is a cajeta int8[16] header. Writes 16 bytes starting at
// offset 8. Caller is responsible for sizing the array correctly.
void __cajeta_md5_finalize_into(void* state, void* out_hdr) {
    if (!state || !out_hdr) return;
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    md5_finalize((struct cajeta_md5_state*) state, out);
}

// Width-named primitive folders. Each writes the value's
// little-endian byte representation. Hasher's contract pins width
// (`writeInt16(1)` and `writeInt32(1)` produce different digests),
// so doing this on the C side avoids per-call temporary array
// allocation on the cajeta side.
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

// finish() Hasher projection: return the first 8 bytes of the digest
// as a little-endian int64. Reads s[0] and s[1] in their post-finalize
// state. NB: this mutates the state (calls md5_finalize), so a second
// finish() returns garbage for the same state — Hasher.finish() is
// terminal by contract.
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

// One-shot variants. Caller pre-allocates the output array on the
// cajeta side (since @Native return of int8[] isn't ABI-bridged in
// v1 — the live-set registration that `new int8[N]` performs doesn't
// flow through a returned-from-C array header). These helpers fill
// the caller's buffer at `out_hdr + 8`.
void __cajeta_md5_oneshot_into(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    struct cajeta_md5_state s;
    md5_init(&s);
    if (data_hdr && len > 0) {
        md5_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    md5_finalize(&s, ((uint8_t*) out_hdr) + 8);
}

// Lowercase hex digest into a caller-supplied int8[32] buffer. The
// cajeta MD5.hashHex wrapper allocates `new int8[32]`, calls this,
// then wraps the array in a String.
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
// Kept in its own reviewable source file and #included here so it rides the
// single-TU runtime -> bitcode -> embed build with NO CMake change (the build
// compiles ONLY cajeta_runtime.c to bitcode; sibling .c files must be textually
// included to be embedded + linker-merged into user modules).
#include "cajeta_sha256.c"
#include "cajeta_blake3.c"

// --- cajeta.hash.SipHash (SipHash-2-4) -------------------------------------
// SipHash-2-4 over arbitrary bytes with a 128-bit key. Designed for
// hash-flooding resistance — exactly the right algorithm when keys
// come from untrusted input (HTTP request bodies, shared cache
// lookups). v1 ships streaming + one-shot; the in-process default
// hasher uses XXH3 instead because SipHash is much slower per byte
// (~2-3 GB/s vs XXH3's ~30 GB/s) — speed beats DoS resistance for
// the in-process internal-keys case.
//
// Reference: Aumasson + Bernstein "SipHash: a fast short-input PRF"
// 2012.

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

// finish() Hasher projection — also the natural digest. SipHash is
// inherently 64-bit so finish() returns the full result.
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
// XXH3-64 from upstream xxhash (already included at the top of this
// file for __cajeta_hash_bytes). We expose alloc/update/digest
// here so the cajeta XXHash3 class has a stable opaque-pointer
// streaming surface; the algorithm is the same one Default Hasher /
// String.hash / @AutoHash all use under the hood.

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

// One-shot XXH3-128, reference path. Writes low64 then high64 (native LE) into
// the 16-byte payload of out_hdr (a Cajeta int8[16]; data starts at +8). Used as
// the correctness oracle for the pure-Cajeta SIMD long path and as the
// small-input (<= 240 B) path that the Cajeta side doesn't vectorize.
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

// Format an XXH3-128 (low64,high64) pair as the 32-char canonical hex digest —
// big-endian high64 then big-endian low64 (matches XXH128_canonicalFromHash) —
// into the 32-byte payload of out_hdr. Lets hash128Hex keep the SIMD crown path
// for the hash itself and only format the two halves here.
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
