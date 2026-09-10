// The `shared` ownership state (slice-spec §3): a heap buffer whose slices escape
// is co-owned by its owner and every view, freed once when the last stake drops.
// The flag is the SIGN BIT of the {i64 count; data} header's count word.

#define CAJETA_SHARED_BIT ((int64_t) 1 << 63)
#define CAJETA_SHARED_INITIAL_CAPACITY (1 << 14)
#define CAJETA_SHARED_TOMBSTONE ((void*) 1)

typedef struct {
    void* base;
    int64_t rc;
} caj_shared_entry;

// The table GROWS by amortized doubling: a fixed table that refused an insert
// once full leaked every buffer promoted after it, bit set but no entry.
static caj_shared_entry* __cajeta_shared_table = NULL;
static int __cajeta_shared_capacity = 0;
static int __cajeta_shared_entries = 0;
static int __cajeta_shared_tombstones = 0;
static pthread_mutex_t __cajeta_shared_mu = PTHREAD_MUTEX_INITIALIZER;

static inline uint64_t caj_shared_hash_cap(const void* p, int cap) {
    return (((uintptr_t) p) >> 4) & (uint64_t) (cap - 1);
}

static inline uint64_t caj_shared_hash(const void* p) {
    return caj_shared_hash_cap(p, __cajeta_shared_capacity);
}

// Live entries + tombstones: they MUST count, or a table of them never rehashes.
static inline int caj_shared_occupancy(void) {
    return __cajeta_shared_entries + __cajeta_shared_tombstones;
}

// Place into a table known to have room; the rehash owns the bookkeeping.
static void caj_shared_place(caj_shared_entry* table, int cap, void* base, int64_t rc) {
    uint64_t idx = caj_shared_hash_cap(base, cap);
    for (;;) {
        caj_shared_entry* e = &table[idx];
        if (e->base == NULL) {
            e->base = base;
            e->rc = rc;
            return;
        }
        idx = (idx + 1) & (uint64_t) (cap - 1);
    }
}

// Grow to `cap`, or rehash in place when tombstones filled it; 0 if calloc fails.
static int caj_shared_rehash_locked(int cap) {
    caj_shared_entry* fresh = (caj_shared_entry*) calloc((size_t) cap, sizeof(caj_shared_entry));
    if (!fresh) return 0;
    if (__cajeta_shared_table) {
        for (int i = 0; i < __cajeta_shared_capacity; i++) {
            caj_shared_entry* e = &__cajeta_shared_table[i];
            if (e->base != NULL && e->base != CAJETA_SHARED_TOMBSTONE) {
                caj_shared_place(fresh, cap, e->base, e->rc);
            }
        }
        free(__cajeta_shared_table);
    }
    __cajeta_shared_table = fresh;
    __cajeta_shared_capacity = cap;
    __cajeta_shared_tombstones = 0;   // dropped by the rehash
    return 1;
}

// Room for one more insert: double past the load factor, rehash for tombstones.
static int caj_shared_reserve_locked(void) {
    if (__cajeta_shared_capacity == 0) {
        return caj_shared_rehash_locked(CAJETA_SHARED_INITIAL_CAPACITY);
    }
    if (caj_shared_occupancy() + 1 <= (__cajeta_shared_capacity * 3) / 4) {
        return 1;
    }
    int target = __cajeta_shared_capacity;
    if (__cajeta_shared_entries + 1 > (__cajeta_shared_capacity * 3) / 8) {
        target = __cajeta_shared_capacity * 2;
        if (target <= 0) return 0;   // capacity overflow; refuse rather than wrap
    }
    return caj_shared_rehash_locked(target);
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
    if (!__cajeta_shared_table) return NULL;
    uint64_t idx = caj_shared_hash(base);
    for (int i = 0; i < __cajeta_shared_capacity; i++) {
        caj_shared_entry* e = &__cajeta_shared_table[idx];
        if (e->base == base) return e;
        if (e->base == NULL) return NULL;
        idx = (idx + 1) & (uint64_t) (__cajeta_shared_capacity - 1);
    }
    return NULL;
}

static void caj_shared_insert_locked(void* base, int64_t rc) {
    if (!caj_shared_reserve_locked()) {
        // Out of memory, not out of table: the buffer keeps its bit and leaks.
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                "cajeta: cannot grow the shared-buffer table (out of memory at "
                "%d entries). Further promotions leak their buffers "
                "(correctness-preserving).\n",
                __cajeta_shared_capacity);
            warned = 1;
        }
        return;
    }
    uint64_t idx = caj_shared_hash(base);
    for (;;) {
        caj_shared_entry* e = &__cajeta_shared_table[idx];
        if (e->base == NULL || e->base == CAJETA_SHARED_TOMBSTONE) {
            if (e->base == CAJETA_SHARED_TOMBSTONE) __cajeta_shared_tombstones--;
            e->base = base;
            e->rc = rc;
            __cajeta_shared_entries++;
            return;
        }
        idx = (idx + 1) & (uint64_t) (__cajeta_shared_capacity - 1);
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

// Promote owned -> shared with `stakes` co-owners; already-shared adds stakes-1.
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

// Take one more stake on a shared buffer; no-op when the base is not shared.
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
    __cajeta_shared_tombstones++;
    return 1;
}

// Drop one stake. Returns 1 iff this was the LAST stake AND the live-set claim
// succeeded, in which case the caller owns the free; a racing claim returns 0.
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

// The owner-drop seam (slice-spec §3.6), called by drop dispatchers for sliceable
// buffers: unshared -> an ordinary claim the caller frees; shared -> release.
int __cajeta_shared_owner_drop(void* base) {
    if (!base) return 0;
    if (*(const int64_t*) base >= 0) {
        return __cajeta_live_set_claim(base);
    }
    return __cajeta_shared_release(base);
}

// C-string view of a String for the legacy const char* ABI. Modes 0/1 hand back
// the data pointer; a windowed view materializes into per-thread scratch.
const char* __cajeta_string_cstr(void* s_v) {
    // A ring, because one call site may collect several results before use.
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
        // Hand the data out directly only for a full-window root that actually
        // CARRIES a terminator: `masked_count > len` is what makes reading
        // base[8 + len] in bounds, so it MUST be tested first.
        if (base && caj_str_off(s) == 0
                && __cajeta_shared_masked_count(base) > len
                && base[8 + len] == '\0') {
            return base + 8;
        }
    }
    // No NUL at a window's end: materialize into the next ring slot.
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

// Escape resolution (slice-spec §4.2) returns a FRESH wrapper the destination owns:
// SSO, arena-rooted and <= 256 B copy; larger heap roots take a stake (mode-2
// window); a static root aliases freely.
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

// --- Slice<T> escape machinery (slice-spec §7.2) ----------------------------
// A Slice<T> VALUE is {T[] store; i64 off; i64 len}: locals are borrows (zero rc),
// escapes resolve in place, copies retain and drops release, all sign-bit gated.

typedef struct {
    void*   store;    // CajetaArray root header {i64 count; data}
    int64_t off;      // element offset from the root's data
    int64_t len;      // window length in elements
} caj_slice_layout;

// Resolve at an escape site: an arena root or a payload <= 256 B copies into a
// FRESH root the destination owns; a larger heap window stakes the root instead.
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

// Copy hook arm: a copy of a RESOLVED slice takes a stake; borrows stay free.
void __cajeta_slice_retain(void* slice_v) {
    caj_slice_layout* s = (caj_slice_layout*) slice_v;
    if (!s || !s->store) return;
    if (*(const int64_t*) s->store < 0) {
        __cajeta_shared_retain(s->store);
    }
}

// Drop hook arm: release a resolved slice's stake, the last one freeing the root.
// Borrows (unshared roots) no-op. Poisons the store against double-release.
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

// Borrow-mode slice: a view the compiler PROVED never leaves its scope takes NO
// stake. A later escape of the VALUE resolves; an SSO source still materializes.
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
    // Normalization (spec §8): a result of <= 12 B is Inline, needing no lifetime.
    if (len <= CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(out, caj_str_ptr(src) + begin, len);
        return out;
    }
    // Larger window of a pointer-form source: a stakeless BORROW window, with the
    // STATIC bit riding along so a consumer never promotes a static root.
    int32_t tag = len | CAJ_STR_BORROW_BIT
        | (src->lenTag & CAJ_STR_STATIC_BIT);
    caj_str_set_window(out, tag, caj_str_off(src) + begin, caj_str_base(src));
    return out;
}

// Zero-copy String.substring (slice-spec §7.1): a mode-2 WINDOWED view whose
// `bytes` stays the ROOT header, its byte offset riding the unused ssoCount field.
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
    // Normalization (spec §8): a result of <= 12 B is Inline — no buffer, no stake.
    if (len <= CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(out, caj_str_ptr(src) + begin, len);
        return out;
    }
    char* base = caj_str_base(src);
    int32_t srcOff = caj_str_off(src);
    // An ARENA-backed root recycles at the scope-exit reset, so a stake on it
    // would dangle: materialize a fresh OWNED root. Static roots are never arena.
    int __cajeta_arena_owns(const void* p);
    if (!(src->lenTag & CAJ_STR_STATIC_BIT) && __cajeta_arena_owns(base)) {
        void* buf = caj_str_new_root(base + 8 + srcOff + begin, len);
        caj_str_set_window(out, len, 0, buf);
        return out;
    }
    // Static roots never enter the shared table: no stake, no free. An owned or
    // borrow-flagged source promotes (owner + this view); a shared one retains.
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
