// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// Exact instrumentation: the sink for the enter/exit probes ProfileCodegen
// emits, counting calls and inclusive time on the method's own descriptor.

// CajetaShadowStack and __cajeta_shadow_ptr come from cajeta_rt_core.c, which
// this file follows in the single-TU include order.

#define CAJ_INSTR_UNLINKED   0   // never seen a probe
#define CAJ_INSTR_LINKED     1   // in the enumeration list
#define CAJ_INSTR_HIDDEN     2   // calibration scratch; never enumerated

// Must match #ProfMethod in src/cajeta/prof/ProfileCodegen.cpp.
typedef struct CajetaProfMethod {
    const char* typeName;
    const char* methodName;
    const char* fileName;
    int64_t     calls;
    int64_t     inclusive_ns;
    int64_t     outside_calls;   // §3.11 — entered with no probed ancestor
    int32_t     registered;      // CAJ_INSTR_*
    int32_t     reserved;
    struct CajetaProfMethod* next;
} CajetaProfMethod;

static CajetaProfMethod*  caj_instr_head = NULL;
static int32_t            caj_instr_count = 0;
static pthread_mutex_t    caj_instr_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int64_t   caj_instr_pairs = 0;
static volatile int       caj_instr_present = 0;
static const char*        caj_instr_selection = "";
static int32_t            caj_instr_opt_level = -1;
static volatile int64_t   caj_instr_probe_ns = -1;   // per pair; <0 = uncalibrated

// Index i is stable only once nothing new registers; a link invalidates it.
static CajetaProfMethod** caj_instr_index = NULL;
static int32_t            caj_instr_index_len = -1;

static void caj_instr_link(CajetaProfMethod* m) {
    pthread_mutex_lock(&caj_instr_mutex);
    if (m->registered == CAJ_INSTR_UNLINKED) {
        m->next = caj_instr_head;
        caj_instr_head = m;
        caj_instr_count++;
        m->registered = CAJ_INSTR_LINKED;
        caj_instr_index_len = -1;
    }
    pthread_mutex_unlock(&caj_instr_mutex);
}

// The probe pair needs the per-fiber shadow stack, so it alone is out of the
// standalone build; tracegen still compiles the rest of this file.
#ifndef CAJETA_PROF_TRACE_STANDALONE

/// Counts the call and returns the span's start timestamp for the exit probe.
int64_t __cajeta_prof_instr_enter(void* handle) {
    CajetaProfMethod* m = (CajetaProfMethod*) handle;
    if (!m) return 0;
    if (m->registered == CAJ_INSTR_UNLINKED) caj_instr_link(m);
    CajetaShadowStack* s = __cajeta_shadow_ptr();
    // Depth 0 means no probed frame is on this fiber's stack, so the call came
    // from outside the selection; attributing it upward would fabricate an edge.
    if (s->instr_depth == 0)
        __atomic_fetch_add(&m->outside_calls, (int64_t) 1, __ATOMIC_RELAXED);
    s->instr_depth++;
    __atomic_fetch_add(&m->calls, (int64_t) 1, __ATOMIC_RELAXED);
    return __cajeta_currentTimeNanos();
}

/// Closes the span opened at `t0`, adding its duration to the descriptor.
void __cajeta_prof_instr_exit(void* handle, int64_t t0) {
    CajetaProfMethod* m = (CajetaProfMethod*) handle;
    if (!m) return;
    const int64_t t1 = __cajeta_currentTimeNanos();
    CajetaShadowStack* s = __cajeta_shadow_ptr();
    if (s->instr_depth > 0) s->instr_depth--;
    if (t1 > t0)
        __atomic_fetch_add(&m->inclusive_ns, t1 - t0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&caj_instr_pairs, (int64_t) 1, __ATOMIC_RELAXED);
}

/// The per-fiber probe depth, exposed like the shadow stack's `top` so the
/// exception machinery can restore it: an unwound frame runs no exit probe, and
/// a depth left too high silently stops every later root call being counted.
int32_t __cajeta_prof_instr_depth(void) {
    return __cajeta_shadow_ptr()->instr_depth;
}
void __cajeta_prof_instr_set_depth(int32_t depth) {
    if (depth >= 0) __cajeta_shadow_ptr()->instr_depth = depth;
}

#endif  /* CAJETA_PROF_TRACE_STANDALONE */

/// Registers a descriptor without running a probe.
void __cajeta_prof_instr_add(void* handle) {
    CajetaProfMethod* m = (CajetaProfMethod*) handle;
    if (m && m->registered == CAJ_INSTR_UNLINKED) caj_instr_link(m);
}

/// Called once per module from a global ctor to publish that probes exist,
/// under which selection, and at which optimization level. Last module wins.
void __cajeta_prof_instr_register_build(const char* selection, int32_t optLevel) {
    caj_instr_present = 1;
    if (selection) caj_instr_selection = selection;
    caj_instr_opt_level = optLevel;
}

int32_t     __cajeta_prof_instr_is_present(void) { return caj_instr_present; }
const char* __cajeta_prof_instr_selection(void) { return caj_instr_selection; }
int32_t     __cajeta_prof_instr_opt_level(void) { return caj_instr_opt_level; }
int64_t     __cajeta_prof_instr_probe_pairs(void) { return caj_instr_pairs; }

static void caj_instr_reindex(void) {
    if (caj_instr_index_len >= 0) return;
    if (caj_instr_index) { free(caj_instr_index); caj_instr_index = NULL; }
    int32_t n = caj_instr_count;
    if (n > 0) {
        caj_instr_index = (CajetaProfMethod**) malloc((size_t) n * sizeof(*caj_instr_index));
        if (!caj_instr_index) { caj_instr_index_len = 0; return; }
        int32_t i = 0;
        for (CajetaProfMethod* m = caj_instr_head; m && i < n; m = m->next)
            caj_instr_index[i++] = m;
        n = i;
    }
    caj_instr_index_len = n;
}

int32_t __cajeta_prof_instr_method_count(void) {
    pthread_mutex_lock(&caj_instr_mutex);
    caj_instr_reindex();
    int32_t n = caj_instr_index_len;
    pthread_mutex_unlock(&caj_instr_mutex);
    return n < 0 ? 0 : n;
}

static CajetaProfMethod* caj_instr_at(int32_t i) {
    pthread_mutex_lock(&caj_instr_mutex);
    caj_instr_reindex();
    CajetaProfMethod* m = (i >= 0 && i < caj_instr_index_len && caj_instr_index)
                        ? caj_instr_index[i] : NULL;
    pthread_mutex_unlock(&caj_instr_mutex);
    return m;
}

const char* __cajeta_prof_instr_method_type(int32_t i) {
    CajetaProfMethod* m = caj_instr_at(i);
    return (m && m->typeName) ? m->typeName : "";
}
const char* __cajeta_prof_instr_method_name(int32_t i) {
    CajetaProfMethod* m = caj_instr_at(i);
    return (m && m->methodName) ? m->methodName : "";
}
const char* __cajeta_prof_instr_method_file(int32_t i) {
    CajetaProfMethod* m = caj_instr_at(i);
    return (m && m->fileName) ? m->fileName : "";
}
int64_t __cajeta_prof_instr_method_calls(int32_t i) {
    CajetaProfMethod* m = caj_instr_at(i);
    return m ? __atomic_load_n(&m->calls, __ATOMIC_RELAXED) : 0;
}
int64_t __cajeta_prof_instr_method_inclusive_ns(int32_t i) {
    CajetaProfMethod* m = caj_instr_at(i);
    return m ? __atomic_load_n(&m->inclusive_ns, __ATOMIC_RELAXED) : 0;
}
int64_t __cajeta_prof_instr_method_outside_calls(int32_t i) {
    CajetaProfMethod* m = caj_instr_at(i);
    return m ? __atomic_load_n(&m->outside_calls, __ATOMIC_RELAXED) : 0;
}

/// Sums `calls` across the descriptors, so no second counter can drift.
int64_t __cajeta_prof_instr_total_calls(void) {
    int64_t total = 0;
    pthread_mutex_lock(&caj_instr_mutex);
    for (CajetaProfMethod* m = caj_instr_head; m; m = m->next)
        total += __atomic_load_n(&m->calls, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&caj_instr_mutex);
    return total;
}

/// Zeroes every counter and the fiber's depth, keeping the registrations.
void __cajeta_prof_instr_reset(void) {
    pthread_mutex_lock(&caj_instr_mutex);
    for (CajetaProfMethod* m = caj_instr_head; m; m = m->next) {
        __atomic_store_n(&m->calls, (int64_t) 0, __ATOMIC_RELAXED);
        __atomic_store_n(&m->inclusive_ns, (int64_t) 0, __ATOMIC_RELAXED);
        __atomic_store_n(&m->outside_calls, (int64_t) 0, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&caj_instr_mutex);
    __atomic_store_n(&caj_instr_pairs, (int64_t) 0, __ATOMIC_RELAXED);
#ifndef CAJETA_PROF_TRACE_STANDALONE
    __cajeta_shadow_ptr()->instr_depth = 0;
#endif
}

// ── §3.5: what the measurement cost ──────────────────────────────────────
// The cheapest round wins: interference can only ever make a round slower.
#define CAJ_INSTR_CALIB_ITERS 20000
#define CAJ_INSTR_CALIB_ROUNDS 5

#ifndef CAJETA_PROF_TRACE_STANDALONE
static CajetaProfMethod caj_instr_calib = {
    "<calibration>", "<probe pair>", "", 0, 0, 0, CAJ_INSTR_HIDDEN, 0, NULL
};
#endif

/// Supplies the per-pair cost for the standalone build, which cannot time it.
void __cajeta_prof_instr_set_probe_ns(int64_t per_pair) {
    __atomic_store_n(&caj_instr_probe_ns, per_pair, __ATOMIC_RELAXED);
}
void __cajeta_prof_instr_set_probe_pairs(int64_t pairs) {
    __atomic_store_n(&caj_instr_pairs, pairs, __ATOMIC_RELAXED);
}

/// The cost of one probe pair in ns, calibrated on first call and cached.
int64_t __cajeta_prof_instr_probe_ns(void) {
    int64_t cached = __atomic_load_n(&caj_instr_probe_ns, __ATOMIC_RELAXED);
    if (cached >= 0) return cached;
#ifdef CAJETA_PROF_TRACE_STANDALONE
    return 0;
#else
    int64_t best = -1;
    for (int r = 0; r < CAJ_INSTR_CALIB_ROUNDS; r++) {
        const int64_t t0 = __cajeta_currentTimeNanos();
        for (int i = 0; i < CAJ_INSTR_CALIB_ITERS; i++) {
            const int64_t e = __cajeta_prof_instr_enter(&caj_instr_calib);
            __cajeta_prof_instr_exit(&caj_instr_calib, e);
        }
        const int64_t t1 = __cajeta_currentTimeNanos();
        const int64_t per = (t1 - t0) / CAJ_INSTR_CALIB_ITERS;
        if (best < 0 || per < best) best = per;
    }
    if (best < 0) best = 0;
    // The calibration's own probes are not the program's.
    __atomic_fetch_sub(&caj_instr_pairs,
                       (int64_t) CAJ_INSTR_CALIB_ITERS * CAJ_INSTR_CALIB_ROUNDS,
                       __ATOMIC_RELAXED);
    caj_instr_calib.calls = 0;
    caj_instr_calib.inclusive_ns = 0;
    caj_instr_calib.outside_calls = 0;
    __atomic_store_n(&caj_instr_probe_ns, best, __ATOMIC_RELAXED);
    return best;
#endif
}

/// Total ns this run spent in probes: pairs times the calibrated pair cost.
int64_t __cajeta_prof_instr_overhead_ns(void) {
    const int64_t per = __cajeta_prof_instr_probe_ns();
    return __atomic_load_n(&caj_instr_pairs, __ATOMIC_RELAXED) * per;
}
