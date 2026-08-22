// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
//
// cajeta-profiler Unit 9 — integrity, tier demotion, teardown
// (spec §10.2, §10.3, §10.4, §11.1, §11.2).
//
// The spec's framing for this whole section is that nearly every failure mode
// found in the research returns success and plausible numbers. So nothing here
// treats "the call succeeded" as evidence: each mechanism drives the state that
// would otherwise be silent and records it where a consumer can read it.
//
// Degradation is never fatal. The ladder floors at HOST because a host
// submit-to-complete window always exists — there is always something honest
// left to report, so a failing tier costs accuracy and never the run (§10.4).

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

// One rung down, never two, and never below HOST. The reason is recorded only
// the first time: a tier that has already fallen accumulates consequences, and
// reporting the last of them would name a symptom instead of the cause.
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

// §11.2 — a backend that accepts every launch and delivers nothing is the
// quietest failure in the system: the trace simply contains no device work,
// which is indistinguishable from a program that launched none. Count the
// silence and act on it.
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

// §11.1 — three checks over a handful of real dispatches, all of which a
// broken backend passes individually:
//   * end exceeds start            — a negative span
//   * duration within a sane bound — a garbage timestamp read as a slow kernel
//   * consecutive dispatches differ — a STUCK counter, which nothing else sees,
//     because every span it produces is perfectly well formed on its own
// A tier that fails any of them is demoted rather than trusted.
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
    // With a single dispatch there is nothing to compare against, so the
    // stuck-counter check does not apply and must not fail the tier.
    if (ok && n > 1 && !distinct) ok = 0;

    if (!ok) __cajeta_prof_tier_demote(domain, CAJETA_DEMOTE_STARTUP_CHECK);
    return ok;
}

// §10.3 — steps unwind in REVERSE. Step 3 may depend on what step 1
// established, so unwinding forwards tears down the ground step 3 is standing
// on. Returns the new depth so a caller can assert its own setup shape.
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

// Run every registered step, newest first, and empty the stack. Emptying it
// FIRST (into a local copy) is what makes a second unwind a no-op rather than a
// double teardown — an unwind triggered from two paths at once is exactly the
// shape a partial initialization produces.
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

    // Outside the lock: a teardown step may legitimately call back into the
    // profiler, and holding the mutex across it would deadlock.
    for (int32_t i = depth - 1; i >= 0; --i) {
        if (fns[i]) fns[i](users[i]);
    }
    return depth;
}

// Initialization succeeded: drop the steps WITHOUT running them. Without this
// half, a successful setup tears itself down.
int32_t __cajeta_prof_undo_commit(int32_t domain) {
    CajTierState* t = caj_tier_at(domain);
    if (!t) return 0;
    pthread_mutex_lock(&caj_tier_mutex);
    int32_t depth = t->undoDepth;
    t->undoDepth = 0;
    pthread_mutex_unlock(&caj_tier_mutex);
    return depth;
}

// §10.2 — absent and inaccessible are different diagnoses with different
// fixes. `access` is the right instrument here despite its TOCTOU reputation:
// this is a diagnostic, not a gate, and it answers for the CALLING user, which
// is precisely the question being asked. stat() would answer about the file.
int32_t __cajeta_prof_probe_node(const char* path) {
    if (!path || !*path) return CAJETA_NODE_ABSENT;
#if defined(_WIN32)
    if (_access(path, 0) != 0) return CAJETA_NODE_ABSENT;
    if (_access(path, 4) != 0) return CAJETA_NODE_INACCESSIBLE;
    return CAJETA_NODE_OK;
#else
    if (access(path, F_OK) != 0) {
        // EACCES here means a directory on the way is closed to us — the node
        // may well exist. Reporting it absent would send the developer to
        // reinstall a driver they already have, which is the exact mistake
        // §10.2 exists to prevent.
        return (errno == EACCES) ? CAJETA_NODE_INACCESSIBLE : CAJETA_NODE_ABSENT;
    }
    if (access(path, R_OK) != 0) return CAJETA_NODE_INACCESSIBLE;
    return CAJETA_NODE_OK;
#endif
}

// Program-lifetime strings: a diagnostic that needed freeing would be dropped
// on the paths that need it most. The permission text names the actual fix
// because "permission denied" alone leaves the developer to guess which of
// several groups a given node wants.
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

// 9.1.a — the device span must sit inside the host submit-to-complete window
// that produced it. This is the one check that catches an entire clock domain
// being wrong: §6.5 measured two backends' preferred domains 5.68 seconds
// apart, and a lane converted with the wrong domain still renders as a
// perfectly ordinary span of a perfectly ordinary duration. Nothing internal to
// the span gives it away — only comparing it against the host window it came
// from does.
//
// A HOST-tier record is exempt from the containment check by construction: its
// device span IS the host window (see the tier note in this header), so
// comparing them tests the assignment, not the clock.
int32_t __cajeta_prof_check_dispatch(const CajetaGpuEvent* ev) {
    if (!ev) return CAJETA_SPAN_UNCORRELATED;
    int32_t flags = CAJETA_SPAN_OK;

    if (ev->dev_end_ns < ev->dev_start_ns) flags |= CAJETA_SPAN_NEGATIVE;
    else if (ev->dev_end_ns - ev->dev_start_ns > CAJETA_SPAN_MAX_NS) {
        flags |= CAJETA_SPAN_IMPLAUSIBLE;
    }

    // A record with no device timing at all is not inconsistent, it is absent;
    // the tier already says which it is.
    if (ev->tier != CAJETA_PROF_TIER_HOST
            && (ev->dev_start_ns != 0 || ev->dev_end_ns != 0)) {
        if (ev->dev_start_ns < ev->host_launch_ns
                || ev->dev_end_ns > ev->host_return_ns) {
            flags |= CAJETA_SPAN_OUTSIDE_HOST;
        }
    }
    return flags;
}
