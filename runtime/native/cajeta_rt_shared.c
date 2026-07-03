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
    if (src->bytes == (void*) &src->ssoCount) {
        void* buf = __cajeta_new_array_header(8, 1, (uint64_t) len + 1);
        *((int64_t*) buf) = len;
        memcpy((char*) buf + 8, (char*) src->bytes + 8 + begin, (size_t) len);
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
        __cajeta_shared_retain(root);
    }
    out->bytes = root;
    out->byteLength = len;
    out->mode = 2;
    out->ssoCount = (int64_t) (srcOff + begin);
    return out;
}
