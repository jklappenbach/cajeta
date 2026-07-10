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
    // Ring of per-thread scratch slots: a call site may collect several
    // cstr results before consuming them (printf with multiple %s args),
    // so one slot would clobber earlier extractions. Eight slots cover
    // realistic arities; beyond that the oldest recycles.
    enum { CAJ_CSTR_RING = 8 };
    static __thread char* scratch[CAJ_CSTR_RING];
    static __thread int64_t cap[CAJ_CSTR_RING];
    static __thread int slot = 0;
    if (!s_v) return NULL;
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    int64_t len = (int64_t) caj_str_len(s);
    if (len == 0) return "";
    if (caj_str_is_pointer(s)) {
        char* base = caj_str_base(s);
        // Full-window root (offset 0, window == the whole root): builders
        // guarantee a trailing NUL — hand the data out directly.
        if (base && caj_str_off(s) == 0
                && len == __cajeta_shared_masked_count(base)) {
            return base + 8;
        }
    }
    // Inline text and windowed views have no NUL at the window's end —
    // materialize into the next ring slot (valid until the ring wraps on
    // this thread; the immediate-consumption ABI satisfies that).
    int k = slot;
    slot = (slot + 1) % CAJ_CSTR_RING;
    if (len + 1 > cap[k]) {
        cap[k] = len + 1 < 64 ? 64 : len + 1;
        scratch[k] = (char*) realloc(scratch[k], (size_t) cap[k]);
    }
    memcpy(scratch[k], caj_str_ptr(s), (size_t) len);
    scratch[k][len] = 0;
    return scratch[k];
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
    int32_t len = caj_str_len(src);
    if (!caj_str_is_pointer(src)) {               // Inline: self-contained
        caj_str_set_inline(out, src->data, len);
        return out;
    }
    char* base = caj_str_base(src);
    int32_t srcOff = caj_str_off(src);
    if (src->lenTag & CAJ_STR_STATIC_BIT) {       // static root: alias freely
        caj_str_set_window(out, len | CAJ_STR_STATIC_BIT, srcOff, base);
        return out;
    }
    int __cajeta_arena_owns(const void* p);
    if (__cajeta_arena_owns(base) || len <= 256) {  // copy-small + arena rows
        void* buf = caj_str_new_root(base + 8 + srcOff, len);
        caj_str_set_window(out, len, 0, buf);       // fresh OWNED root
        return out;
    }
    __cajeta_shared_promote(base, 2);             // share-large: add-or-create
    caj_str_set_window(out, len | CAJ_STR_SHARED_BIT, srcOff, base);
    return out;
}

// --- Slice<T> escape machinery (slice-spec §7.2; slices plan Unit 7b) -----
// A Slice<T> VALUE is {T[] store; i64 off; i64 len} — three words, no
// wrapper. Locals are borrows (zero rc). A slice stored past its scope is
// RESOLVED in place per the §4.2 table; copies of resolved values retain;
// value drops release (both sign-bit-gated, so borrows stay free).

typedef struct {
    void*   store;    // CajetaArray root header {i64 count; data}
    int64_t off;      // element offset from the root's data
    int64_t len;      // window length in elements
} caj_slice_layout;

// Resolve at an escape site (field store): arena root or payload <= 256 B
// copies into a FRESH root the destination owns (rc=1 shared, so the value
// drop's release retires it); a larger heap window takes a stake on the
// root (promote add-or-create: owner + this value). Static-rooted slices
// don't exist today (array literals are heap) — the promote row covers any
// future case safely.
void __cajeta_slice_resolve(void* slice_v, int64_t elemSize) {
    caj_slice_layout* s = (caj_slice_layout*) slice_v;
    if (!s || !s->store || s->len <= 0 || elemSize <= 0) return;
    int64_t payload = s->len * elemSize;
    int __cajeta_arena_owns(const void* p);
    if (__cajeta_arena_owns(s->store) || payload <= 256) {
        void* buf = __cajeta_new_array_header(8, (uint64_t) elemSize,
                                              (uint64_t) s->len);
        *((int64_t*) buf) = s->len;
        memcpy((char*) buf + 8,
               (const char*) s->store + 8 + s->off * elemSize,
               (size_t) payload);
        __cajeta_shared_promote(buf, 1);
        s->store = buf;
        s->off = 0;
        return;
    }
    __cajeta_shared_promote(s->store, 2);
}

// Copy hook arm: a copy of a RESOLVED (shared-rooted) slice holds its own
// stake; borrow copies stay free (sign-bit gate).
void __cajeta_slice_retain(void* slice_v) {
    caj_slice_layout* s = (caj_slice_layout*) slice_v;
    if (!s || !s->store) return;
    if (*(const int64_t*) s->store < 0) {
        __cajeta_shared_retain(s->store);
    }
}

// Drop hook arm: release the stake of a resolved slice; the last stake
// frees the root. Borrows (unshared roots) no-op. Poisons against
// double-release.
void __cajeta_slice_release(void* slice_v) {
    caj_slice_layout* s = (caj_slice_layout*) slice_v;
    if (!s || !s->store) return;
    if (*(const int64_t*) s->store < 0) {
        if (__cajeta_shared_release(s->store)) {
            __cajeta_poison_buffer(s->store);
            free(s->store);
        }
    }
    s->store = NULL;
    s->off = 0;
    s->len = 0;
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
    if (len <= 0 || caj_str_len(src) == 0) {
        caj_str_set_inline(out, NULL, 0);
        return out;
    }
    // Normalization (spec Â§8): every <= 12 B result is Inline — a copy is
    // cheaper than the pointer chase and needs no lifetime at all.
    if (len <= CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(out, caj_str_ptr(src) + begin, len);
        return out;
    }
    // > 12 B window of a pointer-form source (an Inline source can't produce
    // one): stakeless BORROW window — zero rc, zero side-table touch. The
    // STATIC bit rides along so stake-taking consumers of this borrow
    // (string_slice, utf8_of, resolve) never promote a static root.
    int32_t tag = len | CAJ_STR_BORROW_BIT
        | (src->lenTag & CAJ_STR_STATIC_BIT);
    caj_str_set_window(out, tag, caj_str_off(src) + begin, caj_str_base(src));
    return out;
}

void* __cajeta_string_slice(void* src_v, int32_t begin, int32_t len) {
    cajeta_string_layout* src = (cajeta_string_layout*) src_v;
    cajeta_string_layout* out =
        (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
    out->vtable = src->vtable;
    out->cachedCpLength = -1;
    if (len <= 0 || caj_str_len(src) == 0) {
        caj_str_set_inline(out, NULL, 0);
        return out;
    }
    // Normalization (spec Â§8): <= 12 B results are Inline — no buffer, no
    // stake, no rc traffic (this also subsumes the old SSO materialize row).
    if (len <= CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(out, caj_str_ptr(src) + begin, len);
        return out;
    }
    char* base = caj_str_base(src);
    int32_t srcOff = caj_str_off(src);
    // ARENA-backed root (spec Â§4 arena row): the frame arena recycles at
    // the scope-exit reset so a stake on it would dangle — materialize a
    // fresh OWNED root. Static roots are exempt (never arena).
    int __cajeta_arena_owns(const void* p);
    if (!(src->lenTag & CAJ_STR_STATIC_BIT) && __cajeta_arena_owns(base)) {
        void* buf = caj_str_new_root(base + 8 + srcOff + begin, len);
        caj_str_set_window(out, len, 0, buf);
        return out;
    }
    // Static roots never enter the shared table: no stake, no free. A
    // borrow-flagged source holds NO stake: add-or-create (owner + this
    // view). An OWNED source promotes (owner + this view = 2 stakes); a
    // SHARED source retains one more.
    int32_t tag = len;
    if (src->lenTag & CAJ_STR_STATIC_BIT) {
        tag |= CAJ_STR_STATIC_BIT;
    } else if (src->lenTag & CAJ_STR_SHARED_BIT) {
        __cajeta_shared_retain(base);
        tag |= CAJ_STR_SHARED_BIT;
    } else {
        __cajeta_shared_promote(base, 2);   // owned or borrow: add-or-create
        tag |= CAJ_STR_SHARED_BIT;
    }
    caj_str_set_window(out, tag, srcOff + begin, base);
    return out;
}
