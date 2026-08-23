// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
//
// cajeta-profiler Unit 10 — exact instrumentation (spec §3).
//
// The sink for the enter/exit probes ProfileCodegen emits. Sampling answers
// "where does wall time go"; this answers "how many times, and how long
// exactly". They coexist and stay distinguishable by source (§3.4): a sample
// lands in the ring as a stack snapshot, a probe lands here as a counter on the
// method's own descriptor.
//
// Three properties this file is built around:
//
//   ALLOCATION-FREE ON THE PROBE PATH. The counters live INSIDE the codegen-
//   emitted descriptor, so there is no id assignment, no side table, and no
//   hash lookup. The first probe for a method links its descriptor into an
//   enumeration list under a mutex; every probe after that is two relaxed
//   atomic adds and a clock read.
//
//   FIBER-SAFE WITHOUT A SHADOW STACK. `enter` returns its timestamp and the
//   compiler parks it in an alloca in the method's own frame, which travels
//   with the fiber. A yield in the middle of a probed method therefore cannot
//   hand the span to whatever resumed on the same carrier. The one piece of
//   ambient state — the depth used for §3.11 — is per fiber for the same
//   reason, and lives in the shadow stack the runtime already gives each one.
//
//   HONEST ABOUT ITS OWN COST. §3.5 requires the overhead be reported in the
//   trace, so the probe pair is CALIBRATED against this machine and the total
//   is pairs x measured cost. A figure the reader has to guess at is worse than
//   no figure, because it reads exactly like a measured one.
//
// Measured 2026-08-22 on a -O3 native build, 20 M calls: the pair costs ~57 ns,
// and the self-calibration above independently reported 57 against an external
// 56. About 42 ns of that is the two timestamps —
// clock_gettime(CLOCK_MONOTONIC) is 21 ns/call on this machine and a span needs
// one at each end — leaving ~15 ns for the two relaxed atomic adds, the
// per-fiber depth, and the calls. A count-only tier is therefore worth roughly
// 3/4 of the cost, and is where to look before micro-optimizing anything here.

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

// Cached enumeration: the list is built head-first, so index i is only stable
// once nothing new registers. Rebuilt on demand and invalidated by a link.
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

// The probe pair is the one part that needs the runtime's per-fiber shadow
// stack, so it is excluded from the standalone build for the same reason the
// ring drain is (see cajeta_rt_prof_trace.c) — tools/tracegen compiles
// everything else here so CI's trace_processor judges the REAL annotation
// emitter rather than a copy of it.
#ifndef CAJETA_PROF_TRACE_STANDALONE

int64_t __cajeta_prof_instr_enter(void* handle) {
    CajetaProfMethod* m = (CajetaProfMethod*) handle;
    if (!m) return 0;
    if (m->registered == CAJ_INSTR_UNLINKED) caj_instr_link(m);
    CajetaShadowStack* s = __cajeta_shadow_ptr();
    // §3.11. Depth 0 means no probed frame is on this fiber's stack, so this
    // call arrived from outside the selection (or from the runtime, at the
    // program's root). Recorded as a fact about the callee rather than
    // attributed to the nearest probed ancestor, which would be a fabricated
    // call edge.
    if (s->instr_depth == 0)
        __atomic_fetch_add(&m->outside_calls, (int64_t) 1, __ATOMIC_RELAXED);
    s->instr_depth++;
    __atomic_fetch_add(&m->calls, (int64_t) 1, __ATOMIC_RELAXED);
    return __cajeta_currentTimeNanos();
}

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

// §3.11's depth, exposed the way the shadow stack's `top` is, so the exception
// machinery can restore it. An unwound frame never runs its exit probe, so
// without this a throw across N probed frames leaves the depth N too high FOR
// THE REST OF THE FIBER — and since depth 0 is what "my caller was outside the
// selection" means, every later root call silently stops being counted. It
// grows with each throw, so the error compounds rather than washing out. Same
// shape as the 6.4.B lambda leave that eroded the shadow stack a frame per
// call; the fix is the same watermark the drop, shadow and debug chains use.
int32_t __cajeta_prof_instr_depth(void) {
    return __cajeta_shadow_ptr()->instr_depth;
}
void __cajeta_prof_instr_set_depth(int32_t depth) {
    if (depth >= 0) __cajeta_shadow_ptr()->instr_depth = depth;
}

#endif  /* CAJETA_PROF_TRACE_STANDALONE */

// Register a descriptor without running a probe. The standalone build's way in
// (tracegen builds descriptors directly), and harmless in the full runtime.
void __cajeta_prof_instr_add(void* handle) {
    CajetaProfMethod* m = (CajetaProfMethod*) handle;
    if (m && m->registered == CAJ_INSTR_UNLINKED) caj_instr_link(m);
}

// Emitted once per module by a global ctor (ProfileCodegen). Publishes that
// probes exist at all, plus the two facts a reader must never infer: the
// selection that was in force (§3.12) and the optimization level the build used
// (§3.13). Last module wins on a tie, and they agree — every module in one
// build sees the same flags.
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

// Sum of `calls` over every registered method. The exactness claim of §3.1 is
// about this number, so it is read from the descriptors rather than kept as a
// second counter that could drift from them.
int64_t __cajeta_prof_instr_total_calls(void) {
    int64_t total = 0;
    pthread_mutex_lock(&caj_instr_mutex);
    for (CajetaProfMethod* m = caj_instr_head; m; m = m->next)
        total += __atomic_load_n(&m->calls, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&caj_instr_mutex);
    return total;
}

// Zero every counter, keeping the registrations. For a test that wants a known
// call pattern, and for a consumer that profiles a phase rather than a run.
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
//
// Calibrated, not assumed. The probe pair is run against a hidden descriptor
// enough times to swamp the clock's own resolution, and the median-ish figure
// (total / iterations) is cached for the run. A calibration that ran on a
// descheduled core would over-report, so the cheapest of a few rounds wins —
// interference can only ever make a round slower.
#define CAJ_INSTR_CALIB_ITERS 20000
#define CAJ_INSTR_CALIB_ROUNDS 5

#ifndef CAJETA_PROF_TRACE_STANDALONE
static CajetaProfMethod caj_instr_calib = {
    "<calibration>", "<probe pair>", "", 0, 0, 0, CAJ_INSTR_HIDDEN, 0, NULL
};
#endif

// Standalone (tracegen/CI): there is no probe to time, so the figure is
// supplied rather than measured. The annotation emitter downstream is
// identical either way, which is the point.
void __cajeta_prof_instr_set_probe_ns(int64_t per_pair) {
    __atomic_store_n(&caj_instr_probe_ns, per_pair, __ATOMIC_RELAXED);
}
void __cajeta_prof_instr_set_probe_pairs(int64_t pairs) {
    __atomic_store_n(&caj_instr_pairs, pairs, __ATOMIC_RELAXED);
}

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

int64_t __cajeta_prof_instr_overhead_ns(void) {
    const int64_t per = __cajeta_prof_instr_probe_ns();
    return __atomic_load_n(&caj_instr_pairs, __ATOMIC_RELAXED) * per;
}
