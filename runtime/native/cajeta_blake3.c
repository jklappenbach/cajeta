// cajeta_blake3.c — BLAKE3 cryptographic hash, native bridge for cajeta.hash.Blake3.
//
// This is a faithful port of the official BLAKE3 reference implementation
// (the compact, portable, single-file version from the spec appendix): chunk
// chaining + the binary tree of parent nodes, the 7-round compression function,
// and an extendable (XOF) root output. Scalar/portable here — correct on every
// architecture and the oracle for the SIMD path; an AVX-512 hash-many path is
// added separately (gated by a CPUID probe), mirroring the SHA-NI sha256 bridge.
//
// #included by cajeta_runtime.c (single-TU runtime). Symbols are __cajeta_blake3_*.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define B3_OUT_LEN    32
#define B3_KEY_LEN    32
#define B3_BLOCK_LEN  64
#define B3_CHUNK_LEN  1024

#define B3_CHUNK_START (1u << 0)
#define B3_CHUNK_END   (1u << 1)
#define B3_PARENT      (1u << 2)
#define B3_ROOT        (1u << 3)

static const uint32_t B3_IV[8] = {
    0x6A09E667UL, 0xBB67AE85UL, 0x3C6EF372UL, 0xA54FF53AUL,
    0x510E527FUL, 0x9B05688CUL, 0x1F83D9ABUL, 0x5BE0CD19UL,
};

// The 7 per-round message permutations (official MSG_SCHEDULE).
static const uint8_t B3_MSG[7][16] = {
    { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15},
    { 2,  6,  3, 10,  7,  0,  4, 13,  1, 11, 12,  5,  9, 14, 15,  8},
    { 3,  4, 10, 12, 13,  2,  7, 14,  6,  5,  9,  0, 11, 15,  8,  1},
    {10,  7, 12,  9, 14,  3, 13, 15,  4,  0, 11,  2,  5,  8,  1,  6},
    {12, 13,  9, 11, 15, 10, 14,  8,  7,  2,  5,  3,  0,  1,  6,  4},
    { 9, 14, 11,  5,  8, 12, 15,  1, 13,  3,  0, 10,  2,  6,  4,  7},
    {11, 15,  5,  0,  1,  9,  8,  6, 14, 10,  2, 12,  3,  4,  7, 13},
};

static inline uint32_t b3_rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static inline void b3_g(uint32_t* st, int a, int b, int c, int d,
                        uint32_t mx, uint32_t my) {
    st[a] = st[a] + st[b] + mx; st[d] = b3_rotr32(st[d] ^ st[a], 16);
    st[c] = st[c] + st[d];      st[b] = b3_rotr32(st[b] ^ st[c], 12);
    st[a] = st[a] + st[b] + my; st[d] = b3_rotr32(st[d] ^ st[a], 8);
    st[c] = st[c] + st[d];      st[b] = b3_rotr32(st[b] ^ st[c], 7);
}

static void b3_round(uint32_t* st, const uint32_t* m, int r) {
    const uint8_t* s = B3_MSG[r];
    b3_g(st, 0, 4,  8, 12, m[s[0]],  m[s[1]]);
    b3_g(st, 1, 5,  9, 13, m[s[2]],  m[s[3]]);
    b3_g(st, 2, 6, 10, 14, m[s[4]],  m[s[5]]);
    b3_g(st, 3, 7, 11, 15, m[s[6]],  m[s[7]]);
    b3_g(st, 0, 5, 10, 15, m[s[8]],  m[s[9]]);
    b3_g(st, 1, 6, 11, 12, m[s[10]], m[s[11]]);
    b3_g(st, 2, 7,  8, 13, m[s[12]], m[s[13]]);
    b3_g(st, 3, 4,  9, 14, m[s[14]], m[s[15]]);
}

// Full 16-word compression output (out[0..7] = chaining value; out[0..15] are
// used for root/XOF output blocks).
static void b3_compress(const uint32_t cv[8], const uint8_t block[64],
                        uint32_t block_len, uint64_t counter, uint32_t flags,
                        uint32_t out[16]) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) memcpy(&m[i], block + 4 * i, 4); // LE word load
    uint32_t st[16] = {
        cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7],
        B3_IV[0], B3_IV[1], B3_IV[2], B3_IV[3],
        (uint32_t) counter, (uint32_t)(counter >> 32), block_len, flags,
    };
    for (int r = 0; r < 7; r++) b3_round(st, m, r);
    for (int i = 0; i < 8; i++) {
        out[i]     = st[i] ^ st[i + 8];
        out[i + 8] = st[i + 8] ^ cv[i];
    }
}

// --- chunk state ----------------------------------------------------

typedef struct {
    uint32_t cv[8];
    uint64_t chunk_counter;
    uint8_t  block[B3_BLOCK_LEN];
    uint8_t  block_len;
    uint8_t  blocks_compressed;
    uint32_t flags;
} b3_chunk;

static void b3_chunk_reset(b3_chunk* c, const uint32_t key[8], uint64_t counter,
                           uint32_t flags) {
    memcpy(c->cv, key, 32);
    c->chunk_counter = counter;
    memset(c->block, 0, B3_BLOCK_LEN);
    c->block_len = 0;
    c->blocks_compressed = 0;
    c->flags = flags;   // domain-separation flags (0 for plain hashing). MUST be
                        // set — b3_chunk_output/_update read it; leaving it
                        // uninitialized yields stack-garbage-dependent digests.
}

static size_t b3_chunk_len(const b3_chunk* c) {
    return (size_t) B3_BLOCK_LEN * c->blocks_compressed + c->block_len;
}

static uint32_t b3_chunk_start_flag(const b3_chunk* c) {
    return c->blocks_compressed == 0 ? B3_CHUNK_START : 0;
}

// An "output": the compression inputs that produce either a chaining value or
// the root bytes.
typedef struct {
    uint32_t cv[8];
    uint32_t block_words[16];
    uint64_t counter;
    uint32_t block_len;
    uint32_t flags;
} b3_output;

// Outputs are filled via an out-param rather than returned by value (cheaper —
// no large-struct copy on the hot chunk path).
static void b3_chunk_output(const b3_chunk* c, b3_output* o) {
    memcpy(o->cv, c->cv, 32);
    for (int i = 0; i < 16; i++) memcpy(&o->block_words[i], c->block + 4 * i, 4);
    o->counter = c->chunk_counter;
    o->block_len = c->block_len;
    o->flags = c->flags | b3_chunk_start_flag(c) | B3_CHUNK_END;
}

static void b3_output_cv(const b3_output* o, uint32_t cv[8]) {
    uint8_t block[64];
    for (int i = 0; i < 16; i++) memcpy(block + 4 * i, &o->block_words[i], 4);
    uint32_t out[16];
    b3_compress(o->cv, block, o->block_len, o->counter, o->flags, out);
    memcpy(cv, out, 32);
}

// Extendable root output: stream `out_len` bytes, one 64-byte output block per
// counter, with the ROOT flag.
static void b3_output_root(const b3_output* o, uint8_t* out, size_t out_len) {
    uint8_t block[64];
    for (int i = 0; i < 16; i++) memcpy(block + 4 * i, &o->block_words[i], 4);
    uint64_t counter = 0;
    while (out_len > 0) {
        uint32_t words[16];
        b3_compress(o->cv, block, o->block_len, counter, o->flags | B3_ROOT, words);
        for (int i = 0; i < 16 && out_len > 0; i++) {
            uint8_t wb[4];
            memcpy(wb, &words[i], 4);
            for (int j = 0; j < 4 && out_len > 0; j++) { *out++ = wb[j]; out_len--; }
        }
        counter++;
    }
}

static void b3_chunk_update(b3_chunk* c, const uint8_t* input, size_t len) {
    while (len > 0) {
        if (c->block_len == B3_BLOCK_LEN) {
            uint32_t out[16];
            b3_compress(c->cv, c->block, B3_BLOCK_LEN, c->chunk_counter,
                        c->flags | b3_chunk_start_flag(c), out);
            memcpy(c->cv, out, 32);
            c->blocks_compressed++;
            memset(c->block, 0, B3_BLOCK_LEN);
            c->block_len = 0;
        }
        size_t want = B3_BLOCK_LEN - c->block_len;
        size_t take = len < want ? len : want;
        memcpy(c->block + c->block_len, input, take);
        c->block_len += (uint8_t) take;
        input += take;
        len -= take;
    }
}

// Compress two child CVs into a parent output (out-param; see note above).
static void b3_parent_output(const uint32_t left[8], const uint32_t right[8],
                             const uint32_t key[8], uint32_t flags, b3_output* o) {
    memcpy(o->cv, key, 32);
    memcpy(&o->block_words[0], left, 32);
    memcpy(&o->block_words[8], right, 32);
    o->counter = 0;
    o->block_len = B3_BLOCK_LEN;
    o->flags = B3_PARENT | flags;
}

static void b3_parent_cv(const uint32_t left[8], const uint32_t right[8],
                         const uint32_t key[8], uint32_t flags, uint32_t cv[8]) {
    b3_output o;
    b3_parent_output(left, right, key, flags, &o);
    b3_output_cv(&o, cv);
}

// --- hasher ---------------------------------------------------------

typedef struct {
    b3_chunk chunk;
    uint32_t key[8];
    uint32_t cv_stack[8 * 54]; // 54 = max tree depth for 2^64 bytes
    uint8_t  cv_stack_len;     // # of CVs (each 8 words) on the stack
    uint32_t flags;
} b3_hasher;

static void b3_hasher_init(b3_hasher* h) {
    memcpy(h->key, B3_IV, 32);
    h->flags = 0;   // plain hashing: no keyed/derive-key domain flags
    b3_chunk_reset(&h->chunk, h->key, 0, h->flags);
    h->cv_stack_len = 0;
}

static void b3_push_cv(b3_hasher* h, const uint32_t cv[8]) {
    memcpy(&h->cv_stack[(size_t) h->cv_stack_len * 8], cv, 32);
    h->cv_stack_len++;
}

// Add a completed chunk's CV, merging up the tree wherever the total chunk count
// makes a left-subtree complete (popcount trick: merge while counter is even).
static void b3_add_chunk_cv(b3_hasher* h, uint32_t cv[8], uint64_t total_chunks) {
    uint32_t new_cv[8];
    memcpy(new_cv, cv, 32);
    while ((total_chunks & 1) == 0) {
        uint32_t parent[8];
        b3_parent_cv(&h->cv_stack[(size_t)(h->cv_stack_len - 1) * 8], new_cv,
                     h->key, h->flags, parent);
        h->cv_stack_len--;
        memcpy(new_cv, parent, 32);
        total_chunks >>= 1;
    }
    b3_push_cv(h, new_cv);
}

static void b3_hasher_update(b3_hasher* h, const uint8_t* input, size_t len) {
    while (len > 0) {
        if (b3_chunk_len(&h->chunk) == B3_CHUNK_LEN) {
            b3_output o;
            b3_chunk_output(&h->chunk, &o);
            uint32_t cv[8];
            b3_output_cv(&o, cv);
            uint64_t total = h->chunk.chunk_counter + 1;
            b3_add_chunk_cv(h, cv, total);
            b3_chunk_reset(&h->chunk, h->key, total, h->flags);
        }
        size_t want = B3_CHUNK_LEN - b3_chunk_len(&h->chunk);
        size_t take = len < want ? len : want;
        b3_chunk_update(&h->chunk, input, take);
        input += take;
        len -= take;
    }
}

static void b3_hasher_finalize(const b3_hasher* h, uint8_t* out, size_t out_len) {
    b3_output o;
    b3_chunk_output(&h->chunk, &o);
    // Fold the current chunk's output with the CV stack, right to left.
    int parent_nodes_remaining = h->cv_stack_len;
    while (parent_nodes_remaining > 0) {
        parent_nodes_remaining--;
        uint32_t right_cv[8];
        b3_output_cv(&o, right_cv);
        b3_parent_output(&h->cv_stack[(size_t) parent_nodes_remaining * 8],
                         right_cv, h->key, h->flags, &o);
    }
    b3_output_root(&o, out, out_len);
}

// A single chunk's chaining value (no ROOT) — the scalar leaf used by the SIMD
// driver for the tail / sub-16 remainder chunks.
static void b3_scalar_chunk_cv(const uint8_t* input, size_t len, uint64_t counter,
                               uint32_t flags, uint32_t out[8]) {
    b3_chunk ch;
    b3_chunk_reset(&ch, B3_IV, counter, flags);
    b3_chunk_update(&ch, input, len);
    b3_output o;
    b3_chunk_output(&ch, &o);
    b3_output_cv(&o, out);
}

// Reduce chunk CVs [lo,hi) into a single CV via the BLAKE3 tree (non-root
// internal parents). Splits at the largest power-of-two chunk count < (hi-lo).
static void b3_subtree_cv(const uint32_t (*cvs)[8], size_t lo, size_t hi,
                          uint32_t flags, uint32_t out[8]) {
    if (hi - lo == 1) { memcpy(out, cvs[lo], 32); return; }
    size_t n = hi - lo, k = 1;
    while (k * 2 < n) k *= 2;
    uint32_t left[8], right[8];
    b3_subtree_cv(cvs, lo, lo + k, flags, left);
    b3_subtree_cv(cvs, lo + k, hi, flags, right);
    b3_parent_cv(left, right, B3_IV, flags, out);
}

// ====================================================================
//  AVX-512 hash16 — hash 16 chunks in parallel (16x uint32 lanes).
// ====================================================================
#if defined(__x86_64__) || defined(__i386__)
#define CAJETA_BLAKE3_X86 1
#include <immintrin.h>
#include <cpuid.h>

static int cajeta_cpu_has_avx512f(void) {
    static int v = -1;
    if (v >= 0) return v;
    unsigned a, b, c, d;
    int has = 0;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) has = (b & (1u << 16)) != 0; // AVX512F
    v = has;
    return v;
}

__attribute__((target("avx512f")))
static inline void b3_g16(__m512i* v, int a, int b, int c, int d,
                          __m512i mx, __m512i my) {
    v[a] = _mm512_add_epi32(_mm512_add_epi32(v[a], v[b]), mx);
    v[d] = _mm512_ror_epi32(_mm512_xor_si512(v[d], v[a]), 16);
    v[c] = _mm512_add_epi32(v[c], v[d]);
    v[b] = _mm512_ror_epi32(_mm512_xor_si512(v[b], v[c]), 12);
    v[a] = _mm512_add_epi32(_mm512_add_epi32(v[a], v[b]), my);
    v[d] = _mm512_ror_epi32(_mm512_xor_si512(v[d], v[a]), 8);
    v[c] = _mm512_add_epi32(v[c], v[d]);
    v[b] = _mm512_ror_epi32(_mm512_xor_si512(v[b], v[c]), 7);
}

// 16x16 uint32 transpose: out[k] holds word k across all 16 chunks (the message
// schedule layout) from in[L] = chunk L's 16-word block. Standard AVX-512
// unpack/shuffle ladder — far faster than per-word gathers.
__attribute__((target("avx512f")))
static void b3_transpose16(const __m512i in[16], __m512i out[16]) {
    __m512i a[16];
    for (int i = 0; i < 8; i++) {
        a[2 * i]     = _mm512_unpacklo_epi32(in[2 * i], in[2 * i + 1]);
        a[2 * i + 1] = _mm512_unpackhi_epi32(in[2 * i], in[2 * i + 1]);
    }
    __m512i b[16];
    for (int i = 0; i < 4; i++) {
        b[4 * i + 0] = _mm512_unpacklo_epi64(a[4 * i + 0], a[4 * i + 2]);
        b[4 * i + 1] = _mm512_unpackhi_epi64(a[4 * i + 0], a[4 * i + 2]);
        b[4 * i + 2] = _mm512_unpacklo_epi64(a[4 * i + 1], a[4 * i + 3]);
        b[4 * i + 3] = _mm512_unpackhi_epi64(a[4 * i + 1], a[4 * i + 3]);
    }
    __m512i c[16];
    for (int i = 0; i < 2; i++) for (int j = 0; j < 4; j++) {
        c[8 * i + j]     = _mm512_shuffle_i32x4(b[8 * i + j], b[8 * i + j + 4], 0x88);
        c[8 * i + j + 4] = _mm512_shuffle_i32x4(b[8 * i + j], b[8 * i + j + 4], 0xDD);
    }
    for (int i = 0; i < 8; i++) {
        out[i]     = _mm512_shuffle_i32x4(c[i], c[i + 8], 0x88);
        out[i + 8] = _mm512_shuffle_i32x4(c[i], c[i + 8], 0xDD);
    }
}

__attribute__((target("avx512f")))
static void b3_round16(__m512i* v, __m512i* m, int r) {
    const uint8_t* s = B3_MSG[r];
    b3_g16(v, 0, 4,  8, 12, m[s[0]],  m[s[1]]);
    b3_g16(v, 1, 5,  9, 13, m[s[2]],  m[s[3]]);
    b3_g16(v, 2, 6, 10, 14, m[s[4]],  m[s[5]]);
    b3_g16(v, 3, 7, 11, 15, m[s[6]],  m[s[7]]);
    b3_g16(v, 0, 5, 10, 15, m[s[8]],  m[s[9]]);
    b3_g16(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
    b3_g16(v, 2, 7,  8, 13, m[s[12]], m[s[13]]);
    b3_g16(v, 3, 4,  9, 14, m[s[14]], m[s[15]]);
}

// Hash 16 contiguous full (1024-byte) chunks at `input` (chunk L at +L*1024,
// counter = counter_base + L). Writes 16 chaining values into out_cvs.
__attribute__((target("avx512f")))
static void b3_hash16_avx512(const uint8_t* input, uint64_t counter_base,
                             uint32_t base_flags, uint32_t out_cvs[16][8]) {
    __m512i cv[8];
    for (int i = 0; i < 8; i++) cv[i] = _mm512_set1_epi32((int) B3_IV[i]);

    uint32_t lo_arr[16], hi_arr[16];
    for (int L = 0; L < 16; L++) {
        uint64_t cc = counter_base + (uint64_t) L;
        lo_arr[L] = (uint32_t) cc;
        hi_arr[L] = (uint32_t)(cc >> 32);
    }
    __m512i ctr_lo = _mm512_loadu_si512((const void*) lo_arr);
    __m512i ctr_hi = _mm512_loadu_si512((const void*) hi_arr);
    __m512i iv8  = _mm512_set1_epi32((int) B3_IV[0]);
    __m512i iv9  = _mm512_set1_epi32((int) B3_IV[1]);
    __m512i iv10 = _mm512_set1_epi32((int) B3_IV[2]);
    __m512i iv11 = _mm512_set1_epi32((int) B3_IV[3]);
    __m512i blen = _mm512_set1_epi32(B3_BLOCK_LEN);

    for (int blk = 0; blk < 16; blk++) {
        uint32_t flags = base_flags;
        if (blk == 0)  flags |= B3_CHUNK_START;
        if (blk == 15) flags |= B3_CHUNK_END;
        // Load each chunk's 64-byte block, then transpose so m[k] holds word k
        // across all 16 chunks.
        __m512i blocks[16], m[16];
        for (int L = 0; L < 16; L++) {
            blocks[L] = _mm512_loadu_si512(
                (const void*)(input + (size_t) L * B3_CHUNK_LEN + (size_t) blk * 64));
        }
        b3_transpose16(blocks, m);
        __m512i v[16];
        for (int i = 0; i < 8; i++) v[i] = cv[i];
        v[8] = iv8; v[9] = iv9; v[10] = iv10; v[11] = iv11;
        v[12] = ctr_lo; v[13] = ctr_hi; v[14] = blen;
        v[15] = _mm512_set1_epi32((int) flags);
        for (int r = 0; r < 7; r++) b3_round16(v, m, r);
        for (int i = 0; i < 8; i++) cv[i] = _mm512_xor_si512(v[i], v[i + 8]);
    }
    // Un-transpose: out_cvs[L][i] = cv[i] lane L.
    for (int i = 0; i < 8; i++) {
        uint32_t tmp[16];
        _mm512_storeu_si512((void*) tmp, cv[i]);
        for (int L = 0; L < 16; L++) out_cvs[L][i] = tmp[L];
    }
}

// SIMD one-shot driver: len > CHUNK_LEN (>= 2 nodes). Hashes all chunk CVs
// (hash16 for aligned groups of 16, scalar for the <16 remainder + the partial
// tail), then reduces to the ROOT output (XOF-capable via out_len).
static void b3_hash_oneshot_simd(const uint8_t* input, size_t len,
                                 uint8_t* out, size_t out_len) {
    size_t full_chunks = len / B3_CHUNK_LEN;
    size_t tail = len % B3_CHUNK_LEN;
    size_t total = full_chunks + (tail ? 1 : 0);
    uint32_t (*cvs)[8] = (uint32_t (*)[8]) malloc(total * 32);
    if (!cvs) return;
    size_t ci = 0, c = 0;
    while (full_chunks - c >= 16) {
        b3_hash16_avx512(input + c * B3_CHUNK_LEN, (uint64_t) c, 0, &cvs[ci]);
        ci += 16; c += 16;
    }
    while (c < full_chunks) {
        b3_scalar_chunk_cv(input + c * B3_CHUNK_LEN, B3_CHUNK_LEN, (uint64_t) c, 0, cvs[ci]);
        ci++; c++;
    }
    if (tail) {
        b3_scalar_chunk_cv(input + full_chunks * B3_CHUNK_LEN, tail,
                           (uint64_t) full_chunks, 0, cvs[ci]);
        ci++;
    }
    size_t N = total, k = 1;
    while (k * 2 < N) k *= 2;
    uint32_t left[8], right[8];
    b3_subtree_cv((const uint32_t (*)[8]) cvs, 0, k, 0, left);
    b3_subtree_cv((const uint32_t (*)[8]) cvs, k, N, 0, right);
    b3_output o;
    b3_parent_output(left, right, B3_IV, 0, &o);
    b3_output_root(&o, out, out_len);
    free(cvs);
}

static int b3_simd_ok(size_t len) {
    return len > (size_t) B3_CHUNK_LEN && cajeta_cpu_has_avx512f();
}
#else
static int b3_simd_ok(size_t len) { (void) len; return 0; }
static void b3_hash_oneshot_simd(const uint8_t* input, size_t len,
                                 uint8_t* out, size_t out_len) {
    (void) input; (void) len; (void) out; (void) out_len;
}
#endif

// One-shot, dispatching to the AVX-512 path for large inputs.
static void b3_hash_oneshot(const uint8_t* in, size_t len, uint8_t* out, size_t out_len) {
    if (b3_simd_ok(len)) {
        b3_hash_oneshot_simd(in, len, out, out_len);
        return;
    }
    b3_hasher h;
    b3_hasher_init(&h);
    if (len > 0) b3_hasher_update(&h, in, len);
    b3_hasher_finalize(&h, out, out_len);
}

// --- @Native bridges ------------------------------------------------
// Cajeta arrays arrive as a header pointer; the payload starts at +8.

void __cajeta_blake3_oneshot(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    const uint8_t* in = (len > 0 && data_hdr) ? ((const uint8_t*) data_hdr) + 8
                                              : (const uint8_t*) "";
    b3_hash_oneshot(in, (size_t)(len > 0 ? len : 0), ((uint8_t*) out_hdr) + 8, B3_OUT_LEN);
}

// XOF one-shot: fill out_len bytes of extended output.
void __cajeta_blake3_oneshot_xof(const void* data_hdr, int64_t len,
                                 void* out_hdr, int64_t out_len) {
    if (!out_hdr || out_len <= 0) return;
    const uint8_t* in = (len > 0 && data_hdr) ? ((const uint8_t*) data_hdr) + 8
                                              : (const uint8_t*) "";
    b3_hash_oneshot(in, (size_t)(len > 0 ? len : 0),
                    ((uint8_t*) out_hdr) + 8, (size_t) out_len);
}

void __cajeta_blake3_oneshot_hex(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    uint8_t digest[B3_OUT_LEN];
    const uint8_t* in = (len > 0 && data_hdr) ? ((const uint8_t*) data_hdr) + 8
                                              : (const uint8_t*) "";
    b3_hash_oneshot(in, (size_t)(len > 0 ? len : 0), digest, B3_OUT_LEN);
    static const char H[16] = {'0','1','2','3','4','5','6','7',
                               '8','9','a','b','c','d','e','f'};
    uint8_t* o = ((uint8_t*) out_hdr) + 8;
    for (int i = 0; i < B3_OUT_LEN; i++) {
        o[i * 2] = H[digest[i] >> 4]; o[i * 2 + 1] = H[digest[i] & 0xF];
    }
}

// Streaming state: heap-allocate an opaque b3_hasher.
void* __cajeta_blake3_alloc(void) {
    b3_hasher* h = (b3_hasher*) malloc(sizeof(b3_hasher));
    if (h) b3_hasher_init(h);
    return h;
}
void __cajeta_blake3_free(void* st) { if (st) free(st); }
void __cajeta_blake3_reset(void* st) { if (st) b3_hasher_init((b3_hasher*) st); }

void __cajeta_blake3_update(void* st, const void* data_hdr, int64_t len) {
    if (!st || !data_hdr || len <= 0) return;
    b3_hasher_update((b3_hasher*) st, ((const uint8_t*) data_hdr) + 8, (size_t) len);
}

void __cajeta_blake3_digest(void* st, void* out_hdr) {
    if (!st || !out_hdr) return;
    b3_hasher_finalize((b3_hasher*) st, ((uint8_t*) out_hdr) + 8, B3_OUT_LEN);
}

// Project the first 8 digest bytes as a little-endian int64 (Hasher.finish()).
int64_t __cajeta_blake3_finish_int64(void* st) {
    if (!st) return 0;
    uint8_t d[B3_OUT_LEN];
    b3_hasher_finalize((b3_hasher*) st, d, B3_OUT_LEN);
    int64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((int64_t)(uint64_t) d[i]) << (i * 8);
    return v;
}

// Width-named folders — feed the raw LE bytes of a primitive into the hash
// (same approach as MD5 / SipHash). Each is a 1..8-byte update.
void __cajeta_blake3_write_i8(void* st, int8_t v) {
    if (st) b3_hasher_update((b3_hasher*) st, (const uint8_t*) &v, 1);
}
void __cajeta_blake3_write_i16(void* st, int16_t v) {
    if (!st) return;
    uint8_t b[2] = { (uint8_t) v, (uint8_t)(v >> 8) };
    b3_hasher_update((b3_hasher*) st, b, 2);
}
void __cajeta_blake3_write_i32(void* st, int32_t v) {
    if (!st) return;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
    b3_hasher_update((b3_hasher*) st, b, 4);
}
void __cajeta_blake3_write_i64(void* st, int64_t v) {
    if (!st) return;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    b3_hasher_update((b3_hasher*) st, b, 8);
}
void __cajeta_blake3_write_f32(void* st, float v) {
    if (!st) return;
    uint32_t u; memcpy(&u, &v, 4);
    __cajeta_blake3_write_i32(st, (int32_t) u);
}
void __cajeta_blake3_write_f64(void* st, double v) {
    if (!st) return;
    uint64_t u; memcpy(&u, &v, 8);
    __cajeta_blake3_write_i64(st, (int64_t) u);
}
