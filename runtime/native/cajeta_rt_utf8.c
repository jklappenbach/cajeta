// cajeta.lang.Utf8: one 16-byte value overlaid three ways (Inline, Static,
// Shared) and discriminated by the length word, per the forms in
// docs/specification/lang/slice-spec.md. `base` is always a ROOT array header.

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
        return (const char*) &u->off;
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

// Build `out` from a String of any mode. Sources whose bytes can be recycled
// under the value (SSO wrapper regions, arena-backed roots) are materialized
// into a fresh rc=1 root rather than pointed at.
void __cajeta_utf8_of_string(void* out_v, void* s_v) {
    caj_utf8_layout* out = (caj_utf8_layout*) out_v;
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    out->lenTag = 0;
    out->off = 0;
    out->base = NULL;
    if (!s || caj_str_len(s) <= 0) return;
    int32_t len = caj_str_len(s);
    if (len <= CAJ_UTF8_INLINE_CAP) {
        out->lenTag = len;
        memcpy((char*) &out->off, caj_str_ptr(s), (size_t) len);
        return;
    }
    char* base = caj_str_base(s);
    int32_t srcOff = caj_str_off(s);
    int __cajeta_arena_owns(const void* p);
    if (!(s->lenTag & CAJ_STR_STATIC_BIT) && __cajeta_arena_owns(base)) {
        void* buf = caj_str_new_root(base + 8 + srcOff, len);
        __cajeta_shared_promote(buf, 1);
        out->lenTag = len | CAJ_UTF8_SHARED_BIT;
        out->off = 0;
        out->base = (char*) buf;
        return;
    }
    if (s->lenTag & CAJ_STR_STATIC_BIT) {
        out->lenTag = len;
    } else if (s->lenTag & CAJ_STR_SHARED_BIT) {
        __cajeta_shared_retain(base);
        out->lenTag = len | CAJ_UTF8_SHARED_BIT;
    } else {
        __cajeta_shared_promote(base, 2);
        out->lenTag = len | CAJ_UTF8_SHARED_BIT;
    }
    out->off = srcOff;
    out->base = base;
}

// Copy hook arm: one more stake on the same root. No-op for Inline/Static.
void __cajeta_utf8_retain(void* u_v) {
    caj_utf8_layout* u = (caj_utf8_layout*) u_v;
    if (u->lenTag < 0) __cajeta_shared_retain(u->base);
}

// Drop hook arm: release this stake; the last stake frees the root. Poisons the
// value against double-release — a released Utf8 reads back as empty Inline.
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
    if (!s) return n == 0;
    if (n != caj_str_len(s)) return 0;
    if (n == 0) return 1;
    return memcmp(caj_utf8_ptr(u), caj_str_ptr(s), (size_t) n) == 0;
}

// XXH3 over the window bytes — String.hash parity (same core, same seed).
int64_t __cajeta_utf8_hash(void* u_v) {
    caj_utf8_layout* u = (caj_utf8_layout*) u_v;
    int64_t __cajeta_hash_bytes(const uint8_t* data, int64_t len);
    return __cajeta_hash_bytes((const uint8_t*) caj_utf8_ptr(u),
                               (int64_t) caj_utf8_len(u));
}
