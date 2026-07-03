// Utf8 tagged forms — Unit 6b of the slices plan (slice-spec §8).
//
// cajeta.lang.Utf8 is a 16-byte value type. Its .cajeta declaration stays
// {int32 len; int8[12] data}; this file overlays the SAME 16 bytes for the
// pointer forms, discriminated by the length:
//   len <= 12          Inline — bytes live in `data`; pure POD, no rc.
//   len >  12          pointer form {int32 lenTag; int32 off; char* base}:
//     lenTag >= 0        Static — `base` is a never-freed root (a string
//                        literal's array header); no rc.
//     lenTag sign bit    Shared — `base` is an rc'd root in the shared side
//                        table; copies retain, drops release (the count-word
//                        sign-bit convention carried into the value).
// `base` is always a ROOT CajetaArray header (count word + data), never an
// interior pointer — data starts at base+8+off, mirroring mode-2 Strings.
// Normalization (spec §8): every <= 12 B result is built Inline, so pointer
// forms are unambiguous.
//
// Included from cajeta_runtime.c AFTER cajeta_rt_shared.c (shared rc API) and
// cajeta_rt_core.c (cajeta_string_layout, live set, alloc).

#define CAJ_UTF8_INLINE_CAP 12
#define CAJ_UTF8_SHARED_BIT ((int32_t) 1 << 31)

typedef struct {
    int32_t lenTag;       // Inline: len (0..12). Pointer: len | shared bit.
    int32_t off;          // pointer forms: window byte offset from base+8
    char*   base;         // pointer forms: root CajetaArray header
} caj_utf8_layout;

static inline int32_t caj_utf8_len(const caj_utf8_layout* u) {
    return u->lenTag & 0x7FFFFFFF;
}

static inline const char* caj_utf8_ptr(const caj_utf8_layout* u) {
    if (caj_utf8_len(u) <= CAJ_UTF8_INLINE_CAP)
        return (const char*) &u->off;      // Inline: data starts at byte 4
    return u->base + 8 + u->off;
}

int32_t __cajeta_utf8_size(void* u_v) {
    return caj_utf8_len((caj_utf8_layout*) u_v);
}

int8_t __cajeta_utf8_byte_at(void* u_v, int32_t idx) {
    caj_utf8_layout* u = (caj_utf8_layout*) u_v;
    if (idx < 0 || idx >= caj_utf8_len(u)) return 0;
    return (int8_t) caj_utf8_ptr(u)[idx];
}

// Build `out` from a String (any mode), lifting 6a's 12-byte clamp:
//   total <= 12               -> Inline copy (normalization rule).
//   > 12, SSO wrapper region  -> materialize an owned root, rc=1 Shared (a
//                                wrapper's inline bytes must never be pointed
//                                at — §8.3 invariant).
//   > 12, ARENA-backed root   -> materialize, rc=1 Shared (spec §4 arena row:
//                                the frame arena recycles at scope reset, so
//                                its buffers can never back a stake).
//   > 12, mode 0 (owned)      -> promote(root, 2): owner + this stake; Shared.
//   > 12, mode 2 (windowed)   -> retain(root); offsets accumulate; Shared.
//   > 12, mode 1 (static)     -> Static; no rc.
void __cajeta_utf8_of_string(void* out_v, void* s_v) {
    caj_utf8_layout* out = (caj_utf8_layout*) out_v;
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    out->lenTag = 0;
    out->off = 0;
    out->base = NULL;
    if (!s || s->bytes == NULL || s->byteLength <= 0) return;
    int32_t len = s->byteLength;
    int32_t srcOff = (s->mode == 2) ? (int32_t) s->ssoCount : 0;
    const char* src = (const char*) s->bytes + 8 + srcOff;
    if (len <= CAJ_UTF8_INLINE_CAP) {
        out->lenTag = len;
        memcpy((char*) &out->off, src, (size_t) len);
        return;
    }
    int __cajeta_arena_owns(const void* p);
    if (s->bytes == (void*) &s->ssoCount
            || (s->mode != 1 && __cajeta_arena_owns(s->bytes))) {
        void* buf = __cajeta_new_array_header(8, 1, (uint64_t) len + 1);
        *((int64_t*) buf) = len;
        memcpy((char*) buf + 8, src, (size_t) len);
        ((char*) buf)[8 + len] = 0;
        __cajeta_shared_promote(buf, 1);
        out->lenTag = len | CAJ_UTF8_SHARED_BIT;
        out->off = 0;
        out->base = (char*) buf;
        return;
    }
    void* root = s->bytes;
    if (s->mode == 0) {
        __cajeta_shared_promote(root, 2);
        out->lenTag = len | CAJ_UTF8_SHARED_BIT;
    } else if (s->mode == 2) {
        if (s->ssoData[0]) {
            __cajeta_shared_promote(root, 2);   // borrow source holds no stake
        } else {
            __cajeta_shared_retain(root);
        }
        out->lenTag = len | CAJ_UTF8_SHARED_BIT;
    } else {
        out->lenTag = len;                 // Static: no rc
    }
    out->off = srcOff;
    out->base = (char*) root;
}

// Copy hook arm: one more stake on the same root. No-op for Inline/Static.
void __cajeta_utf8_retain(void* u_v) {
    caj_utf8_layout* u = (caj_utf8_layout*) u_v;
    if (u->lenTag < 0) __cajeta_shared_retain(u->base);
}

// Drop hook arm: release this stake; last stake frees the root (same contract
// as __cajeta_string_drop's mode-2 branch). Poisons the value against
// double-release (a released Utf8 reads as empty Inline).
void __cajeta_utf8_release(void* u_v) {
    caj_utf8_layout* u = (caj_utf8_layout*) u_v;
    if (u->lenTag < 0 && u->base != NULL) {
        if (__cajeta_shared_release(u->base)) {
            __cajeta_poison_buffer(u->base);
            free(u->base);
        }
    }
    u->lenTag = 0;
    u->off = 0;
    u->base = NULL;
}

int32_t __cajeta_utf8_equals(void* a_v, void* b_v) {
    caj_utf8_layout* a = (caj_utf8_layout*) a_v;
    caj_utf8_layout* b = (caj_utf8_layout*) b_v;
    int32_t n = caj_utf8_len(a);
    if (n != caj_utf8_len(b)) return 0;
    if (n == 0) return 1;
    return memcmp(caj_utf8_ptr(a), caj_utf8_ptr(b), (size_t) n) == 0;
}

int32_t __cajeta_utf8_equals_string(void* u_v, void* s_v) {
    caj_utf8_layout* u = (caj_utf8_layout*) u_v;
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    int32_t n = caj_utf8_len(u);
    if (!s || s->bytes == NULL) return n == 0;
    if (n != s->byteLength) return 0;
    if (n == 0) return 1;
    int32_t srcOff = (s->mode == 2) ? (int32_t) s->ssoCount : 0;
    return memcmp(caj_utf8_ptr(u), (const char*) s->bytes + 8 + srcOff,
                  (size_t) n) == 0;
}

// XXH3 over the window bytes — String.hash parity (same core, same seed).
int64_t __cajeta_utf8_hash(void* u_v) {
    caj_utf8_layout* u = (caj_utf8_layout*) u_v;
    int64_t __cajeta_hash_bytes(const uint8_t* data, int64_t len);
    return __cajeta_hash_bytes((const uint8_t*) caj_utf8_ptr(u),
                               (int64_t) caj_utf8_len(u));
}
