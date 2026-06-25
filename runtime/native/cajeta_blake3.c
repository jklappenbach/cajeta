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

// --- @Native bridges ------------------------------------------------
// Cajeta arrays arrive as a header pointer; the payload starts at +8.

void __cajeta_blake3_oneshot(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    const uint8_t* in = (len > 0 && data_hdr) ? ((const uint8_t*) data_hdr) + 8
                                              : (const uint8_t*) "";
    b3_hasher h;
    b3_hasher_init(&h);
    if (len > 0) b3_hasher_update(&h, in, (size_t) len);
    b3_hasher_finalize(&h, ((uint8_t*) out_hdr) + 8, B3_OUT_LEN);
}

// XOF one-shot: fill out_len bytes of extended output.
void __cajeta_blake3_oneshot_xof(const void* data_hdr, int64_t len,
                                 void* out_hdr, int64_t out_len) {
    if (!out_hdr || out_len <= 0) return;
    const uint8_t* in = (len > 0 && data_hdr) ? ((const uint8_t*) data_hdr) + 8
                                              : (const uint8_t*) "";
    b3_hasher h;
    b3_hasher_init(&h);
    if (len > 0) b3_hasher_update(&h, in, (size_t) len);
    b3_hasher_finalize(&h, ((uint8_t*) out_hdr) + 8, (size_t) out_len);
}

void __cajeta_blake3_oneshot_hex(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    uint8_t digest[B3_OUT_LEN];
    const uint8_t* in = (len > 0 && data_hdr) ? ((const uint8_t*) data_hdr) + 8
                                              : (const uint8_t*) "";
    b3_hasher h;
    b3_hasher_init(&h);
    if (len > 0) b3_hasher_update(&h, in, (size_t) len);
    b3_hasher_finalize(&h, digest, B3_OUT_LEN);
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
