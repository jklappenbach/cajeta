// ============================================================================
// The `shared` ownership state (docs/specification/lang/slice-spec.md §3).
//
// A heap buffer whose slices escape their owner is promoted owned -> shared:
// co-owned by its remaining owner and every escaped slice, freed exactly once
// when the last stake drops. The flag is the SIGN BIT of the {i64 count; data}
// header's count word — no layout change, and any unmasked reader of a shared
// buffer's count sees a negative value and fails CLOSED (bounds abort), never
// an overrun. The count of stakes lives in a side table keyed by buffer base,
// mirroring the live-set (open-addressed, tombstoned, mt-gated). The final
// release routes through __cajeta_live_set_claim so a racing auto-field-drop
// still no-ops (the double-free guard is preserved under the new last-drop
// protocol).
// ============================================================================

#define CAJETA_SHARED_BIT ((int64_t) 1 << 63)
#define CAJETA_SHARED_CAPACITY (1 << 14)
#define CAJETA_SHARED_LOAD_CAP ((CAJETA_SHARED_CAPACITY * 3) / 4)
#define CAJETA_SHARED_TOMBSTONE ((void*) 1)

typedef struct {
    void* base;
    int64_t rc;
} caj_shared_entry;

static caj_shared_entry __cajeta_shared_table[CAJETA_SHARED_CAPACITY];
static int __cajeta_shared_entries = 0;
static pthread_mutex_t __cajeta_shared_mu = PTHREAD_MUTEX_INITIALIZER;

static inline uint64_t caj_shared_hash(const void* p) {
    return (((uintptr_t) p) >> 4) & (CAJETA_SHARED_CAPACITY - 1);
}

int64_t __cajeta_shared_population(void) {
    return (int64_t) __cajeta_shared_entries;
}

// Sign-bit test on the count word; no table lookup on the unshared fast path.
int __cajeta_shared_is(const void* base) {
    if (!base) return 0;
    return *(const int64_t*) base < 0;
}

// Masked element count of a possibly-shared {i64 count; data} buffer.
int64_t __cajeta_shared_masked_count(const void* base) {
    if (!base) return 0;
    return *(const int64_t*) base & ~CAJETA_SHARED_BIT;
}

static caj_shared_entry* caj_shared_find_locked(void* base) {
    uint64_t idx = caj_shared_hash(base);
    for (int i = 0; i < CAJETA_SHARED_CAPACITY; i++) {
        caj_shared_entry* e = &__cajeta_shared_table[idx];
        if (e->base == base) return e;
        if (e->base == NULL) return NULL;
        idx = (idx + 1) & (CAJETA_SHARED_CAPACITY - 1);
    }
    return NULL;
}

static void caj_shared_insert_locked(void* base, int64_t rc) {
    if (__cajeta_shared_entries >= CAJETA_SHARED_LOAD_CAP) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                "cajeta: shared-buffer table reached load cap (%d). Further "
                "promotions leak their buffers (correctness-preserving).\n",
                CAJETA_SHARED_LOAD_CAP);
            warned = 1;
        }
        return;
    }
    uint64_t idx = caj_shared_hash(base);
    for (;;) {
        caj_shared_entry* e = &__cajeta_shared_table[idx];
        if (e->base == NULL || e->base == CAJETA_SHARED_TOMBSTONE) {
            e->base = base;
            e->rc = rc;
            __cajeta_shared_entries++;
            return;
        }
        idx = (idx + 1) & (CAJETA_SHARED_CAPACITY - 1);
    }
}

static void caj_shared_promote_locked(void* base, int64_t stakes) {
    caj_shared_entry* e = caj_shared_find_locked(base);
    if (e) {
        // Already shared (a slice of a shared buffer): the new view retains.
        e->rc += stakes - 1;
        return;
    }
    *(int64_t*) base |= CAJETA_SHARED_BIT;
    caj_shared_insert_locked(base, stakes);
}

// Promote owned -> shared with `stakes` co-owners (typically 2: owner + the
// escaping view). Idempotent-forgiving: promoting an already-shared buffer
// adds stakes-1 (the owner's stake is already counted).
void __cajeta_shared_promote(void* base, int64_t stakes) {
    if (!base) return;
    if (__atomic_load_n(&__cajeta_live_set_mt, __ATOMIC_ACQUIRE) == 0) {
        caj_shared_promote_locked(base, stakes);
        return;
    }
    pthread_mutex_lock(&__cajeta_shared_mu);
    caj_shared_promote_locked(base, stakes);
    pthread_mutex_unlock(&__cajeta_shared_mu);
}

void __cajeta_shared_retain(void* base) {
    if (!base) return;
    if (__atomic_load_n(&__cajeta_live_set_mt, __ATOMIC_ACQUIRE) == 0) {
        caj_shared_entry* e = caj_shared_find_locked(base);
        if (e) e->rc++;
        return;
    }
    pthread_mutex_lock(&__cajeta_shared_mu);
    caj_shared_entry* e = caj_shared_find_locked(base);
    if (e) e->rc++;
    pthread_mutex_unlock(&__cajeta_shared_mu);
}

// Test/introspection: current stake count, or -1 if the base is not shared.
int64_t __cajeta_shared_rc(void* base) {
    int64_t r = -1;
    if (__atomic_load_n(&__cajeta_live_set_mt, __ATOMIC_ACQUIRE) == 0) {
        caj_shared_entry* e = caj_shared_find_locked(base);
        if (e) r = e->rc;
        return r;
    }
    pthread_mutex_lock(&__cajeta_shared_mu);
    caj_shared_entry* e = caj_shared_find_locked(base);
    if (e) r = e->rc;
    pthread_mutex_unlock(&__cajeta_shared_mu);
    return r;
}

static int caj_shared_release_locked(void* base) {
    caj_shared_entry* e = caj_shared_find_locked(base);
    if (!e) return 0;
    e->rc--;
    if (e->rc > 0) return 0;
    e->base = CAJETA_SHARED_TOMBSTONE;
    __cajeta_shared_entries--;
    return 1;
}

// Drop one stake. Returns 1 iff this was the LAST stake AND the live-set claim
// succeeded — the caller then owns the free (same contract as a drop
// dispatcher's claim). A racing claim by an auto-field-drop makes this return
// 0 and the buffer is not touched.
int __cajeta_shared_release(void* base) {
    if (!base) return 0;
    int last;
    if (__atomic_load_n(&__cajeta_live_set_mt, __ATOMIC_ACQUIRE) == 0) {
        last = caj_shared_release_locked(base);
    } else {
        pthread_mutex_lock(&__cajeta_shared_mu);
        last = caj_shared_release_locked(base);
        pthread_mutex_unlock(&__cajeta_shared_mu);
    }
    if (!last) return 0;
    return __cajeta_live_set_claim(base);
}

// The owner-drop seam (slice-spec §3.6): drop dispatchers for sliceable
// buffers call this instead of a bare claim. Unshared (sign bit clear, the
// overwhelmingly common path) -> ordinary claim, caller frees as today.
// Shared -> the owner's stake releases; the buffer outlives into its slices.
int __cajeta_shared_owner_drop(void* base) {
    if (!base) return 0;
    if (*(const int64_t*) base >= 0) {
        return __cajeta_live_set_claim(base);
    }
    return __cajeta_shared_release(base);
}

// C-string view of a String for the legacy const char* runtime ABI
// (println/parse/log). Modes 0/1: the data pointer directly (writers guarantee
// a trailing NUL). Mode 2 (windowed view): the window has no NUL at its end,
// so materialize into a per-thread growable scratch — valid until the next
// call on the same thread, which the immediate-consumption ABI satisfies.
const char* __cajeta_string_cstr(void* s_v) {
    static __thread char* scratch = NULL;
    static __thread int64_t cap = 0;
    if (!s_v) return NULL;
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    if (s->bytes == NULL) return "";
    if (s->mode != 2) return (const char*) s->bytes + 8;
    int64_t len = (int64_t) s->byteLength;
    if (len + 1 > cap) {
        cap = len + 1 < 64 ? 64 : len + 1;
        scratch = (char*) realloc(scratch, (size_t) cap);
    }
    memcpy(scratch, (const char*) s->bytes + 8 + s->ssoCount, (size_t) len);
    scratch[len] = 0;
    return scratch;
}

// Zero-copy String.substring (slice-spec §7.1; slices plan 2.2.1). Builds a
// mode-2 WINDOWED view: `bytes` stays the ROOT array header (bounds checks
// remain valid), the window's byte offset rides the otherwise-unused ssoCount
// field, and readers add `off = (mode==2) ? (int32) ssoCount : 0`.
//   SSO source        -> materialized owned copy (never view a wrapper's
//                        inline region — §8.3 invariant).
//   mode-0 source     -> promote(root, 2): owner + this view.
//   mode-2 source     -> retain(root); offsets accumulate (chained substrings
//                        attribute to the root).
//   mode-1 source     -> no rc (a static/borrowed root is never written; the
//                        release at drop no-ops on unregistered roots).
// Escape resolution (slice-spec §4.2; slices plan Unit 4). Called at an
// escape site (a plain String field store whose RHS is a scope-owned local
// wrapper): returns a FRESH wrapper the destination owns — the stored value
// no longer aliases the source's wrapper (whose declaring scope frees it),
// and the §4.2 row decides the backing:
//   SSO / arena root / len <= threshold  -> materialized owned copy (mode 0)
//   large heap root                      -> stake on the root (promote-or-
//                                           retain; mode-2 window)
//   static root (mode 1)                 -> free alias (no rc, mode 1)
// [D-thresh] = 256 B.
void* __cajeta_string_resolve(void* src_v) {
    cajeta_string_layout* src = (cajeta_string_layout*) src_v;
    if (!src) return NULL;
    cajeta_string_layout* out =
        (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
    out->vtable = src->vtable;
    out->cachedCpLength = src->cachedCpLength;
    out->ssoCount = 0;
    memset(out->ssoData, 0, sizeof out->ssoData);
    int32_t len = src->byteLength;
    if (len <= 0 || src->bytes == NULL) {
        out->bytes = NULL;
        out->byteLength = 0;
        out->mode = 0;
        return out;
    }
    int32_t srcOff = (src->mode == 2) ? (int32_t) src->ssoCount : 0;
    if (src->mode == 1) {                         // static root: alias freely
        out->bytes = src->bytes;
        out->byteLength = len;
        out->mode = 1;
        return out;
    }
    int __cajeta_arena_owns(const void* p);
    if (src->bytes == (void*) &src->ssoCount
            || __cajeta_arena_owns(src->bytes)
            || len <= 256) {                      // copy-small + arena + SSO rows
        void* buf = __cajeta_new_array_header(8, 1, (uint64_t) len + 1);
        *((int64_t*) buf) = len;
        memcpy((char*) buf + 8, (char*) src->bytes + 8 + srcOff, (size_t) len);
        ((char*) buf)[8 + len] = 0;
        out->bytes = buf;
        out->byteLength = len;
        out->mode = 0;
        return out;
    }
    __cajeta_shared_promote(src->bytes, 2);       // share-large: add-or-create
    out->bytes = src->bytes;
    out->byteLength = len;
    out->mode = 2;
    out->ssoCount = (int64_t) srcOff;
    return out;
}

// Borrow-mode slice (slices plan 4.2.2 local-borrow downgrade): for a slice
// the compiler PROVED never leaves its scope (no return, no `#`-move, no
// lambda capture — field stores and call args resolve at their own sites),
// the view takes NO stake at all: zero rc traffic, zero side-table touch.
// The wrapper is an ordinary mode-2 window whose drop's release no-ops on an
// unregistered root; any later escape of the VALUE resolves (promote is
// add-or-create). SSO sources still materialize (never alias a wrapper's
// inline region); arena/heap/static roots alias freely — the borrow dies
// with its scope, before its root.
void* __cajeta_string_slice_borrow(void* src_v, int32_t begin, int32_t len) {
    cajeta_string_layout* src = (cajeta_string_layout*) src_v;
    cajeta_string_layout* out =
        (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
    out->vtable = src->vtable;
    out->cachedCpLength = -1;
    out->ssoCount = 0;
    memset(out->ssoData, 0, sizeof out->ssoData);
    if (len <= 0 || src->bytes == NULL) {
        out->bytes = NULL;
        out->byteLength = 0;
        out->mode = 0;
        return out;
    }
    int32_t srcOff = (src->mode == 2) ? (int32_t) src->ssoCount : 0;
    if (src->bytes == (void*) &src->ssoCount) {
        void* buf = __cajeta_new_array_header(8, 1, (uint64_t) len + 1);
        *((int64_t*) buf) = len;
        memcpy((char*) buf + 8, (char*) src->bytes + 8 + srcOff + begin, (size_t) len);
        ((char*) buf)[8 + len] = 0;
        out->bytes = buf;
        out->byteLength = len;
        out->mode = 0;
        return out;
    }
    out->bytes = src->bytes;
    out->byteLength = len;
    out->mode = 2;
    out->ssoCount = (int64_t) (srcOff + begin);
    // The borrow flag: ssoData[0] (unused when bytes != &ssoCount). A
    // stakeless mode-2 wrapper must NOT release at drop — once an escape
    // registers the root, an unflagged borrow's drop would steal a stake
    // it never took.
    out->ssoData[0] = 1;
    return out;
}

void* __cajeta_string_slice(void* src_v, int32_t begin, int32_t len) {
    cajeta_string_layout* src = (cajeta_string_layout*) src_v;
    cajeta_string_layout* out =
        (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
    out->vtable = src->vtable;
    out->cachedCpLength = -1;
    out->ssoCount = 0;
    memset(out->ssoData, 0, sizeof out->ssoData);
    if (len <= 0 || src->bytes == NULL) {
        out->bytes = NULL;
        out->byteLength = 0;
        out->mode = 0;
        return out;
    }
    int32_t srcOff = (src->mode == 2) ? (int32_t) src->ssoCount : 0;
    // Materialize (owned copy) when the root can't back a stake: SSO (the
    // window lives in the wrapper's inline region — §8.3 invariant) or an
    // ARENA-backed root (spec §4 arena row: the frame arena recycles at the
    // scope-exit reset so a stake on it would dangle, and its reclaim never
    // routes owner_drop so the rc entry could never retire). Zero-copy
    // stands for heap-backed roots.
    int __cajeta_arena_owns(const void* p);
    if (src->bytes == (void*) &src->ssoCount
            || (src->mode != 1 && __cajeta_arena_owns(src->bytes))) {
        void* buf = __cajeta_new_array_header(8, 1, (uint64_t) len + 1);
        *((int64_t*) buf) = len;
        memcpy((char*) buf + 8, (char*) src->bytes + 8 + srcOff + begin, (size_t) len);
        ((char*) buf)[8 + len] = 0;
        out->bytes = buf;
        out->byteLength = len;
        out->mode = 0;
        return out;
    }
    void* root = src->bytes;
    if (src->mode == 0) {
        __cajeta_shared_promote(root, 2);
    } else if (src->mode == 2) {
        // A borrow-flagged source holds NO stake: add-or-create (owner +
        // this view) instead of retaining a possibly-absent entry.
        if (src->ssoData[0]) {
            __cajeta_shared_promote(root, 2);
        } else {
            __cajeta_shared_retain(root);
        }
    }
    out->bytes = root;
    out->byteLength = len;
    out->mode = 2;
    out->ssoCount = (int64_t) (srcOff + begin);
    return out;
}
