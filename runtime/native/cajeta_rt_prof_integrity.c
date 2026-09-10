// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// Integrity, tier demotion and teardown. Nothing here treats "the call
// succeeded" as evidence, and degradation is never fatal: the ladder floors at
// HOST, where a submit-to-complete window always exists.

typedef struct {
    int32_t tier;            // CAJETA_PROF_TIER_*
    int32_t reason;          // CAJETA_DEMOTE_*; the FIRST one, see below
    int32_t launches;        // dispatches accepted since the last reset
    int64_t records;         // records actually delivered

    CajetaUndoFn undoFn[CAJETA_UNDO_MAX_STEPS];
    void*        undoUser[CAJETA_UNDO_MAX_STEPS];
    int32_t      undoDepth;
} CajTierState;

static CajTierState caj_tier[CAJETA_CLOCK_MAX_DOMAINS];
static int32_t caj_tier_record_threshold = CAJETA_TIER_RECORD_THRESHOLD;
static pthread_mutex_t caj_tier_mutex = PTHREAD_MUTEX_INITIALIZER;

static CajTierState* caj_tier_at(int32_t domain) {
    if (domain < 0 || domain >= CAJETA_CLOCK_MAX_DOMAINS) return NULL;
    return &caj_tier[domain];
}

int32_t __cajeta_prof_tier_reset(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    if (!t) return 0;
    pthread_mutex_lock(&caj_tier_mutex);
    memset(t, 0, sizeof(*t));
    t->tier = CAJETA_PROF_TIER_DEVICE;
    t->reason = CAJETA_DEMOTE_NONE;
    pthread_mutex_unlock(&caj_tier_mutex);
    return 1;
}

int32_t __cajeta_prof_tier(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    return t ? t->tier : CAJETA_PROF_TIER_HOST;
}

int32_t __cajeta_prof_tier_reason(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    return t ? t->reason : CAJETA_DEMOTE_NONE;
}

/// Drops one rung, never below HOST; only the FIRST reason is kept.
int32_t __cajeta_prof_tier_demote(int32_t domain, int32_t reason) {
    CajTierState* t = caj_tier_at(domain);
    if (!t) return CAJETA_PROF_TIER_HOST;
    pthread_mutex_lock(&caj_tier_mutex);
    if (t->tier < CAJETA_PROF_TIER_HOST) t->tier++;
    if (t->reason == CAJETA_DEMOTE_NONE) t->reason = reason;
    int32_t now = t->tier;
    pthread_mutex_unlock(&caj_tier_mutex);
    return now;
}

int32_t __cajeta_prof_tier_set_record_threshold(int32_t launches) {
    if (launches <= 0) return 0;
    __atomic_store_n(&caj_tier_record_threshold, launches, __ATOMIC_RELEASE);
    return 1;
}

/// Counts a dispatch, demoting once enough are accepted with no record back.
int32_t __cajeta_prof_tier_note_launch(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    if (!t) return 0;
    int32_t demote = 0;
    pthread_mutex_lock(&caj_tier_mutex);
    t->launches++;
    if (t->records == 0
            && t->launches >= __atomic_load_n(&caj_tier_record_threshold,
                                              __ATOMIC_ACQUIRE)) {
        demote = 1;
    }
    pthread_mutex_unlock(&caj_tier_mutex);
    // Outside the lock: demote takes the same mutex.
    if (demote && __cajeta_prof_tier(domain) == CAJETA_PROF_TIER_DEVICE) {
        __cajeta_prof_tier_demote(domain, CAJETA_DEMOTE_NO_RECORDS);
    }
    return 1;
}

int32_t __cajeta_prof_tier_note_records(int32_t domain, int64_t n) {
    CajTierState* t = caj_tier_at(domain);
    if (!t || n <= 0) return 0;
    pthread_mutex_lock(&caj_tier_mutex);
    t->records += n;
    pthread_mutex_unlock(&caj_tier_mutex);
    return 1;
}

int32_t __cajeta_prof_tier_launches(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    return t ? t->launches : 0;
}

int64_t __cajeta_prof_tier_records(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    return t ? t->records : 0;
}

/// Checks `n` dispatches for negative spans, implausible durations and a STUCK
/// counter, demoting the tier on any failure. Returns 1 when all three pass.
int32_t __cajeta_prof_tier_verify(int32_t domain, const int64_t* startsNs,
                                  const int64_t* endsNs, int32_t n) {
    if (!startsNs || !endsNs || n <= 0) return 0;
    if (!caj_tier_at(domain)) return 0;

    int32_t ok = 1;
    int32_t distinct = 0;
    for (int32_t i = 0; i < n; ++i) {
        if (endsNs[i] < startsNs[i]) { ok = 0; break; }
        if (endsNs[i] - startsNs[i] > CAJETA_SPAN_MAX_NS) { ok = 0; break; }
        if (i > 0 && (startsNs[i] != startsNs[i - 1] || endsNs[i] != endsNs[i - 1])) {
            distinct = 1;
        }
    }
    // One dispatch has nothing to compare against; it must not fail the tier.
    if (ok && n > 1 && !distinct) ok = 0;

    if (!ok) __cajeta_prof_tier_demote(domain, CAJETA_DEMOTE_STARTUP_CHECK);
    return ok;
}

/// Pushes one teardown step, returning the new depth so a caller can assert
/// its setup shape. 0 when the stack is full.
int32_t __cajeta_prof_undo_push(int32_t domain, CajetaUndoFn fn, void* user) {
    CajTierState* t = caj_tier_at(domain);
    if (!t || !fn) return 0;
    pthread_mutex_lock(&caj_tier_mutex);
    int32_t depth = t->undoDepth;
    if (depth >= CAJETA_UNDO_MAX_STEPS) {
        pthread_mutex_unlock(&caj_tier_mutex);
        return 0;
    }
    t->undoFn[depth] = fn;
    t->undoUser[depth] = user;
    t->undoDepth = depth + 1;
    depth = t->undoDepth;
    pthread_mutex_unlock(&caj_tier_mutex);
    return depth;
}

int32_t __cajeta_prof_undo_depth(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    return t ? t->undoDepth : 0;
}

/// Runs every step newest first and empties the stack. Emptying it FIRST, into
/// a local copy, is what makes a second unwind a no-op, not a double teardown.
int32_t __cajeta_prof_undo_unwind(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    if (!t) return 0;
    CajetaUndoFn fns[CAJETA_UNDO_MAX_STEPS];
    void* users[CAJETA_UNDO_MAX_STEPS];
    int32_t depth;

    pthread_mutex_lock(&caj_tier_mutex);
    depth = t->undoDepth;
    for (int32_t i = 0; i < depth; ++i) {
        fns[i] = t->undoFn[i];
        users[i] = t->undoUser[i];
    }
    t->undoDepth = 0;
    pthread_mutex_unlock(&caj_tier_mutex);

    // Outside the lock: a step may call back into the profiler and deadlock.
    for (int32_t i = depth - 1; i >= 0; --i) {
        if (fns[i]) fns[i](users[i]);
    }
    return depth;
}

/// Drops the steps WITHOUT running them, for an initialization that succeeded.
int32_t __cajeta_prof_undo_commit(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    if (!t) return 0;
    pthread_mutex_lock(&caj_tier_mutex);
    int32_t depth = t->undoDepth;
    t->undoDepth = 0;
    pthread_mutex_unlock(&caj_tier_mutex);
    return depth;
}

/// CAJETA_NODE_OK, _ABSENT or _INACCESSIBLE for a device node. `access` despite
/// its TOCTOU reputation: this is a diagnostic, and it answers for THIS user.
int32_t __cajeta_prof_probe_node(const char* path) {
    if (!path || !*path) return CAJETA_NODE_ABSENT;
#if defined(_WIN32)
    if (_access(path, 0) != 0) return CAJETA_NODE_ABSENT;
    if (_access(path, 4) != 0) return CAJETA_NODE_INACCESSIBLE;
    return CAJETA_NODE_OK;
#else
    if (access(path, F_OK) != 0) {
        // EACCES means a directory on the way is closed to us, so the node may
        // well exist; calling it absent sends the developer to reinstall.
        return (errno == EACCES) ? CAJETA_NODE_INACCESSIBLE : CAJETA_NODE_ABSENT;
    }
    if (access(path, R_OK) != 0) return CAJETA_NODE_INACCESSIBLE;
    return CAJETA_NODE_OK;
#endif
}

/// Advice for a probe_node status, as a program-lifetime string.
const char* __cajeta_prof_node_advice(int32_t status, const char* path) {
    (void) path;
    switch (status) {
        case CAJETA_NODE_ABSENT:
            return "device node not present: the driver is not loaded, or the "
                   "device is not installed. Check that the kernel module is "
                   "loaded (lsmod) before enabling device timing.";
        case CAJETA_NODE_INACCESSIBLE:
            return "device node present but not readable by this user: add "
                   "yourself to the owning group (typically `render` or "
                   "`video`, e.g. `sudo usermod -aG render $USER`) and start a "
                   "new login session.";
        default:
            return "";
    }
}

/// The CAJETA_SPAN_* flags for one dispatch: its device span must sit inside
/// the CAUSAL bracket [host submit, record RESOLUTION] — bounding by
/// host_return_ns would flag healthy async spans. HOST-tier records are exempt.
int32_t __cajeta_prof_check_dispatch(const CajetaGpuEvent* ev) {
    if (!ev) return CAJETA_SPAN_UNCORRELATED;
    int32_t flags = CAJETA_SPAN_OK;

    if (ev->dev_end_ns < ev->dev_start_ns) flags |= CAJETA_SPAN_NEGATIVE;
    else if (ev->dev_end_ns - ev->dev_start_ns > CAJETA_SPAN_MAX_NS) {
        flags |= CAJETA_SPAN_IMPLAUSIBLE;
    }

    // No device timing at all is absence, not inconsistency.
    if (ev->tier != CAJETA_PROF_TIER_HOST
            && (ev->dev_start_ns != 0 || ev->dev_end_ns != 0)) {
        const int64_t upper = ev->resolved_ns ? ev->resolved_ns
                                              : ev->host_return_ns;
        if (ev->dev_start_ns < ev->host_launch_ns
                || ev->dev_end_ns > upper) {
            flags |= CAJETA_SPAN_OUTSIDE_HOST;
        }
        // TIER_DEVICE claims a vendor record supplied this span; with no
        // resolution behind it, the timestamps have no provenance.
        if (ev->tier == CAJETA_PROF_TIER_DEVICE && ev->resolved_ns == 0) {
            flags |= CAJETA_SPAN_UNCORRELATED;
        }
    }
    return flags;
}
