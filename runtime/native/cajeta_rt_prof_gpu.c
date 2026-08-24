// === cajeta-profiler Unit 7 — the GPU dispatch record seam (spec §5.1, §5.3,
// === §5.6). #included into cajeta_runtime.c AFTER cajeta_rt_prof_trace.c (it
// === uses the writer) and BEFORE cajeta_xpu.c (the launch path calls in here).
//
// The seam is DUAL-CONSUMER by construction (spec §14.9). Records are published
// to registered sinks; the Perfetto writer is one of them, and the XPU
// scheduler's feedback loop (xpu-kernel-scheduling §3, §8) will be another,
// consuming the same records live. Wiring the writer in as the collector's
// callee would have been less code today and a rewrite of every backend's
// collection path later.
//
// Three properties this file exists to keep, in the order they are easy to lose:
//
//   1. A slow or broken consumer must not become backpressure on the program
//      under test (§5.6.4). Publication is bounded and drops; it never waits.
//   2. Sinks must not be able to reach each other (§5.6.2). Each gets its own
//      copy in its own queue — not a shared buffer with a shared cursor.
//   3. With nothing registered, the dispatch path must be what it was (7.1.e).
//      That is one relaxed load of `g_gpu_sinks_live` and a branch.

#include <pthread.h>

int64_t __cajeta_currentTimeNanos(void);   // cajeta_rt_concurrent_exec.c, later in this TU

// ── per-sink queue ────────────────────────────────────────────────────────
//
// Vyukov-style bounded MPMC ring, used multi-producer / single-consumer: many
// threads dispatch concurrently (7.1.d), one delivery thread drains. The
// per-slot sequence number is what makes a producer's claim and its write
// separable, so a claim that is still in flight is invisible to the consumer
// rather than delivering a half-written record.
//
// A mutex would have been shorter. It would also have made a slow consumer able
// to stall a dispatching thread, which is exactly the property §5.6.4 forbids —
// and it would have failed only under load, i.e. never in a test.
typedef struct {
    volatile int64_t seq;
    CajetaGpuEvent   ev;
} CajGpuSlot;

typedef struct {
    CajGpuSlot*      ring;
    int64_t          mask;          // capacity - 1; capacity is a power of two
    volatile int64_t head;          // producers claim here
    volatile int64_t tail;          // the consumer drains here
    volatile int64_t dropped;
    volatile int64_t delivered;
    CajetaGpuSinkFn  fn;
    void*            user;
    int32_t          granularity;
    volatile int32_t enabled;
    volatile int32_t in_use;
} CajGpuSink;

static CajGpuSink       g_gpu_sink[CAJETA_GPU_MAX_SINKS];
static pthread_mutex_t  g_gpu_reg_lock = PTHREAD_MUTEX_INITIALIZER;
// Held while a sink is being CALLED, so a synchronous flush and the delivery
// thread cannot enter the same sink at once. Producers never take it — that is
// the whole point.
static pthread_mutex_t  g_gpu_deliver_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int32_t g_gpu_sinks_live = 0;   // the dispatch-path fast path
static volatile int64_t g_gpu_launch_id  = 0;
static volatile int64_t g_gpu_records    = 0;
static int32_t          g_gpu_queue_cap  = CAJETA_GPU_SINK_QUEUE;

static int64_t caj_gpu_pow2(int64_t v) {
    int64_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

static int32_t caj_gpu_queue_init(CajGpuSink* s, int32_t cap) {
    int64_t n = caj_gpu_pow2(cap > 0 ? cap : CAJETA_GPU_SINK_QUEUE);
    s->ring = (CajGpuSlot*) calloc((size_t) n, sizeof(CajGpuSlot));
    if (!s->ring) return 0;
    for (int64_t i = 0; i < n; i++) s->ring[i].seq = i;
    s->mask = n - 1;
    s->head = 0;
    s->tail = 0;
    return 1;
}

// Never blocks. Returns 1 if the record was queued, 0 if the queue was full —
// in which case the caller counts a drop rather than waiting (§5.6.4).
static int32_t caj_gpu_enqueue(CajGpuSink* s, const CajetaGpuEvent* ev) {
    int64_t pos = __atomic_load_n(&s->head, __ATOMIC_RELAXED);
    for (;;) {
        CajGpuSlot* slot = &s->ring[pos & s->mask];
        int64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        int64_t d = seq - pos;
        if (d == 0) {
            if (__atomic_compare_exchange_n(&s->head, &pos, pos + 1, 1,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                slot->ev = *ev;      // each sink's OWN copy: §5.6.2 by construction
                __atomic_store_n(&slot->seq, pos + 1, __ATOMIC_RELEASE);
                return 1;
            }
        } else if (d < 0) {
            return 0;                // full
        } else {
            pos = __atomic_load_n(&s->head, __ATOMIC_RELAXED);
        }
    }
}

static int32_t caj_gpu_dequeue(CajGpuSink* s, CajetaGpuEvent* out) {
    int64_t pos = s->tail;           // single consumer, under g_gpu_deliver_lock
    CajGpuSlot* slot = &s->ring[pos & s->mask];
    int64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if (seq - (pos + 1) != 0) return 0;   // empty, or a producer still writing
    *out = slot->ev;
    s->tail = pos + 1;
    __atomic_store_n(&slot->seq, pos + s->mask + 1, __ATOMIC_RELEASE);
    return 1;
}

// ── delivery ──────────────────────────────────────────────────────────────

#define CAJ_GPU_BATCH_MAX 64

// Drain one sink and hand what came out to it. Returns the number of records
// delivered. Called with g_gpu_deliver_lock held.
static int32_t caj_gpu_drain_one(CajGpuSink* s) {
    if (!s->in_use || !s->enabled || !s->fn) return 0;
    CajetaGpuEvent batch[CAJ_GPU_BATCH_MAX];
    int32_t total = 0;
    for (;;) {
        int32_t n = 0;
        while (n < CAJ_GPU_BATCH_MAX && caj_gpu_dequeue(s, &batch[n])) {
            n++;
            // A per-record sink asked to see them one at a time (§5.6.8): hand
            // over immediately rather than accumulating and calling it N times
            // with a stale batch, which would honor the LETTER of the
            // granularity and none of the latency it was asked for.
            if (s->granularity == CAJETA_GPU_SINK_PER_RECORD) break;
        }
        if (n == 0) return total;
        int32_t rc = s->fn(batch, n, s->user);
        if (rc != 0) {
            // §5.6.5: isolate, disable, report. The run continues.
            s->enabled = 0;
            fprintf(stderr,
                    "cajeta.profiler: record sink faulted (rc=%d) after %lld "
                    "records; disabling it. The run and the remaining sinks "
                    "continue.\n",
                    (int) rc, (long long) s->delivered);
            return total;
        }
        s->delivered += n;
        total += n;
    }
}

// Deliver everything queued, on the caller's thread. Returns the number of
// sinks that received at least one record.
int32_t __cajeta_prof_gpu_flush(void) {
    int32_t touched = 0;
    pthread_mutex_lock(&g_gpu_deliver_lock);
    for (int32_t i = 0; i < CAJETA_GPU_MAX_SINKS; i++)
        if (caj_gpu_drain_one(&g_gpu_sink[i]) > 0) touched++;
    pthread_mutex_unlock(&g_gpu_deliver_lock);
    return touched;
}

static pthread_t        g_gpu_deliver_thread;
static volatile int32_t g_gpu_deliver_running = 0;
static volatile int32_t g_gpu_deliver_stop = 0;

// Per-record sinks want promptness; batched sinks explicitly do not (§14.12 —
// the writer is filling a file). So the thread polls at a per-record cadence and
// leaves batched queues to accumulate until they are half full or somebody
// flushes. That is also what makes the granularity test deterministic instead of
// a race against a timer.
static void* caj_gpu_deliver_loop(void* arg) {
    (void) arg;
    while (!g_gpu_deliver_stop) {
        pthread_mutex_lock(&g_gpu_deliver_lock);
        for (int32_t i = 0; i < CAJETA_GPU_MAX_SINKS; i++) {
            CajGpuSink* s = &g_gpu_sink[i];
            if (!s->in_use || !s->enabled) continue;
            if (s->granularity == CAJETA_GPU_SINK_PER_RECORD) {
                caj_gpu_drain_one(s);
            } else {
                int64_t pending = __atomic_load_n(&s->head, __ATOMIC_RELAXED) - s->tail;
                if (pending > (s->mask + 1) / 2) caj_gpu_drain_one(s);
            }
        }
        pthread_mutex_unlock(&g_gpu_deliver_lock);
        struct timespec ts = { 0, 200 * 1000 };   // 200us
        nanosleep(&ts, NULL);
    }
    return NULL;
}

// ── sink registry ─────────────────────────────────────────────────────────

int32_t __cajeta_prof_gpu_sink_register(CajetaGpuSinkFn fn, void* user,
                                        int32_t granularity) {
    if (!fn) return -1;
    // Anything that is not an explicit per-record request is "undeclared", and
    // an undeclared sink gets batched — the cheaper default (§5.6.8).
    if (granularity != CAJETA_GPU_SINK_PER_RECORD)
        granularity = CAJETA_GPU_SINK_BATCHED;
    pthread_mutex_lock(&g_gpu_reg_lock);
    int32_t id = -1;
    for (int32_t i = 0; i < CAJETA_GPU_MAX_SINKS; i++) {
        if (!g_gpu_sink[i].in_use) { id = i; break; }
    }
    if (id < 0) { pthread_mutex_unlock(&g_gpu_reg_lock); return -1; }
    CajGpuSink* s = &g_gpu_sink[id];
    if (!caj_gpu_queue_init(s, g_gpu_queue_cap)) {
        pthread_mutex_unlock(&g_gpu_reg_lock);
        return -1;
    }
    s->fn = fn;
    s->user = user;
    s->granularity = granularity;
    s->dropped = 0;
    s->delivered = 0;
    s->enabled = 1;
    __atomic_store_n(&s->in_use, 1, __ATOMIC_RELEASE);
    // Registering ARMS the seam. That is §5.6.3 in one rule: a live consumer
    // does not imply a trace file, and (below) a trace file registers a sink of
    // its own, so neither direction implies the other.
    __atomic_add_fetch(&g_gpu_sinks_live, 1, __ATOMIC_RELEASE);
    if (!g_gpu_deliver_running) {
        g_gpu_deliver_stop = 0;
        if (pthread_create(&g_gpu_deliver_thread, NULL, caj_gpu_deliver_loop, NULL) == 0)
            g_gpu_deliver_running = 1;
    }
    pthread_mutex_unlock(&g_gpu_reg_lock);
    return id;
}

int32_t __cajeta_prof_gpu_sink_unregister(int32_t id) {
    if (id < 0 || id >= CAJETA_GPU_MAX_SINKS) return 0;
    pthread_mutex_lock(&g_gpu_reg_lock);
    CajGpuSink* s = &g_gpu_sink[id];
    if (!s->in_use) { pthread_mutex_unlock(&g_gpu_reg_lock); return 0; }
    // Stop publication first, then take the delivery lock: a sink must not be
    // freed while the delivery thread is inside it.
    __atomic_store_n(&s->in_use, 0, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&g_gpu_sinks_live, 1, __ATOMIC_RELEASE);
    int32_t last = (__atomic_load_n(&g_gpu_sinks_live, __ATOMIC_ACQUIRE) == 0);
    if (last && g_gpu_deliver_running) {
        g_gpu_deliver_stop = 1;
        pthread_join(g_gpu_deliver_thread, NULL);
        g_gpu_deliver_running = 0;
    }
    pthread_mutex_lock(&g_gpu_deliver_lock);
    free(s->ring);
    s->ring = NULL;
    s->fn = NULL;
    s->user = NULL;
    s->enabled = 0;
    pthread_mutex_unlock(&g_gpu_deliver_lock);
    pthread_mutex_unlock(&g_gpu_reg_lock);
    return 1;
}

int32_t __cajeta_prof_gpu_sink_count(void) {
    return __atomic_load_n(&g_gpu_sinks_live, __ATOMIC_ACQUIRE);
}
int32_t __cajeta_prof_gpu_sink_enabled(int32_t id) {
    if (id < 0 || id >= CAJETA_GPU_MAX_SINKS) return 0;
    return g_gpu_sink[id].in_use && g_gpu_sink[id].enabled;
}
int64_t __cajeta_prof_gpu_sink_dropped(int32_t id) {
    if (id < 0 || id >= CAJETA_GPU_MAX_SINKS) return 0;
    return __atomic_load_n(&g_gpu_sink[id].dropped, __ATOMIC_ACQUIRE);
}
int64_t __cajeta_prof_gpu_sink_delivered(int32_t id) {
    if (id < 0 || id >= CAJETA_GPU_MAX_SINKS) return 0;
    return g_gpu_sink[id].delivered;
}
int32_t __cajeta_prof_gpu_sink_granularity(int32_t id) {
    if (id < 0 || id >= CAJETA_GPU_MAX_SINKS) return -1;
    return g_gpu_sink[id].granularity;
}
// Queue capacity for sinks registered AFTER this call. Exists so a test can
// force the full-queue path with eight slots instead of by generating a
// thousand launches — a drop that only happens under production load is a drop
// nobody ever asserts.
int32_t __cajeta_prof_gpu_set_queue_cap(int32_t cap) {
    if (cap <= 0) return g_gpu_queue_cap;
    g_gpu_queue_cap = cap;
    return cap;
}
int32_t __cajeta_prof_gpu_is_armed(void)     { return __cajeta_prof_gpu_sink_count() > 0; }
int64_t __cajeta_prof_gpu_records(void)      { return __atomic_load_n(&g_gpu_records, __ATOMIC_ACQUIRE); }
int64_t __cajeta_prof_gpu_last_launch_id(void) { return __atomic_load_n(&g_gpu_launch_id, __ATOMIC_ACQUIRE); }

// Publish one completed record to every registered sink. Bounded, never blocks.
void __cajeta_prof_gpu_publish(const CajetaGpuEvent* ev) {
    if (!ev) return;
    __atomic_add_fetch(&g_gpu_records, 1, __ATOMIC_RELAXED);
    for (int32_t i = 0; i < CAJETA_GPU_MAX_SINKS; i++) {
        CajGpuSink* s = &g_gpu_sink[i];
        if (!__atomic_load_n(&s->in_use, __ATOMIC_ACQUIRE) || !s->enabled) continue;
        if (!caj_gpu_enqueue(s, ev))
            __atomic_add_fetch(&s->dropped, 1, __ATOMIC_RELAXED);
    }
}

// ── backends ──────────────────────────────────────────────────────────────

// CPU emulation (spec §5.3): device times come from host wall time, which for a
// synchronous dispatch IS the kernel's span. It reports TIER_HOST anyway — see
// the tier note in cajeta_prof_abi.h. Its value is that the whole pipeline,
// seam to trace, is exercisable with no GPU and no vendor library (§5.3.2).
static int32_t caj_gpu_cpu_init(void) { return 1; }
static int32_t caj_gpu_cpu_begin(CajetaGpuEvent* ev) {
    ev->dev_start_ns = __cajeta_currentTimeNanos();
    ev->tier = CAJETA_PROF_TIER_HOST;
    return 1;
}
static int32_t caj_gpu_cpu_end(CajetaGpuEvent* ev) {
    ev->dev_end_ns = __cajeta_currentTimeNanos();
    return 1;
}
// Nothing is buffered and there is no device clock, so there is nothing to
// drain and nothing to correlate. Present rather than absent so the vtable is
// the same shape every backend fills (plan 7.3.a).
static int32_t caj_gpu_cpu_collect(void)   { return 0; }
static int32_t caj_gpu_cpu_calibrate(void) { return 1; }

static const CajetaGpuBackendVtbl caj_gpu_cpu_vtbl = {
    "cpu", caj_gpu_cpu_init, caj_gpu_cpu_begin, caj_gpu_cpu_end,
    caj_gpu_cpu_collect, caj_gpu_cpu_calibrate
};

// ROCm (Unit 8, spec §5.2). Selected only once rocprofiler-sdk is actually
// bound; until then backend 1 takes the host lane below, which is what makes
// §5.2.2's "degraded and reported" true rather than aspirational. The vtable
// deliberately does not exist in a half-bound form — a rocm backend answering
// with zeros would be worse than the host window, because a zero device span
// is indistinguishable downstream from a measured one.
static int32_t caj_gpu_rocm_init(void) { return __cajeta_prof_rocm_init(); }

// ── the pending table (8.2.c) ─────────────────────────────────────────────
//
// A dispatch record arrives after the launch that caused it has already
// returned — often several launches later, in a batch. So a launch that is
// waiting for its device span is PARKED here instead of being published at the
// seam, and the record claims it by launch id when it turns up.
//
// The alternative — flushing and waiting inside end_launch — would make every
// launch synchronous with its own completion. Two streams that genuinely
// overlapped would be recorded back to back, and §5.1.3 asks for exactly the
// opposite. Waiting is the one thing a profiler of concurrency must not do.
//
// Bounded, and full is not fatal: an unparkable launch publishes immediately at
// host tier, which is a true measurement of a narrower thing. Nothing is
// dropped for want of a slot.
#define CAJ_GPU_PENDING_MAX 256

typedef struct {
    int32_t        in_use;
    int64_t        launch_id;
    CajetaGpuEvent ev;
} CajGpuPending;

static CajGpuPending   g_gpu_pending[CAJ_GPU_PENDING_MAX];
static pthread_mutex_t g_gpu_pending_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t         g_gpu_pending_overflow = 0;   // published at host tier instead
static int64_t         g_gpu_pending_unclaimed = 0;  // parked, never matched, flushed out

// Park a launch awaiting its device record. Returns 1 when parked (the caller
// must NOT publish), 0 when the table is full (the caller publishes as-is).
static int32_t caj_gpu_park(const CajetaGpuEvent* ev) {
    int i;
    pthread_mutex_lock(&g_gpu_pending_lock);
    for (i = 0; i < CAJ_GPU_PENDING_MAX; ++i) {
        if (g_gpu_pending[i].in_use) continue;
        g_gpu_pending[i].in_use    = 1;
        g_gpu_pending[i].launch_id = ev->launch_id;
        g_gpu_pending[i].ev        = *ev;
        pthread_mutex_unlock(&g_gpu_pending_lock);
        return 1;
    }
    g_gpu_pending_overflow++;
    pthread_mutex_unlock(&g_gpu_pending_lock);
    return 0;
}

int32_t __cajeta_prof_gpu_resolve_dispatch(int64_t launchId,
                                           int64_t devStartNs, int64_t devEndNs) {
    CajetaGpuEvent ev;
    int i;
    int found = 0;

    pthread_mutex_lock(&g_gpu_pending_lock);
    for (i = 0; i < CAJ_GPU_PENDING_MAX; ++i) {
        if (!g_gpu_pending[i].in_use || g_gpu_pending[i].launch_id != launchId) continue;
        ev = g_gpu_pending[i].ev;
        g_gpu_pending[i].in_use = 0;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_gpu_pending_lock);
    if (!found) return 0;   // not ours, or already resolved — never invent one

    ev.dev_start_ns = devStartNs;
    ev.dev_end_ns   = devEndNs;
    // TIER_DEVICE is claimed here and nowhere else in this backend. It means a
    // vendor dispatch record supplied the span; every other path through the
    // ROCm backend reports HOST, because that is what those numbers are.
    ev.tier = CAJETA_PROF_TIER_DEVICE;
    // The record's arrival closes the span's causal bracket: a real execution
    // ended before the record describing it was read. The integrity check
    // bounds dev_end by this rather than by host_return_ns, which an
    // asynchronous dispatch overruns by construction (plan 6.7.2.c).
    ev.resolved_ns = __cajeta_currentTimeNanos();
    __cajeta_prof_gpu_publish(&ev);   // published OUTSIDE the lock: a sink runs
                                      // user code, and holding a lock across it
                                      // would let a slow sink stall every launch
    return 1;
}

// Publish everything still parked, at host tier. Called when the trace ends and
// after a flush that did not claim everything: a launch whose record never came
// back still has an honest host submit-to-complete window, and losing it
// entirely would be worse than reporting it for what it is.
static int32_t caj_gpu_drain_pending(void) {
    CajetaGpuEvent batch[CAJ_GPU_PENDING_MAX];
    int32_t n = 0;
    int i;

    pthread_mutex_lock(&g_gpu_pending_lock);
    for (i = 0; i < CAJ_GPU_PENDING_MAX; ++i) {
        if (!g_gpu_pending[i].in_use) continue;
        batch[n++] = g_gpu_pending[i].ev;
        g_gpu_pending[i].in_use = 0;
    }
    g_gpu_pending_unclaimed += n;
    pthread_mutex_unlock(&g_gpu_pending_lock);

    for (i = 0; i < n; ++i) __cajeta_prof_gpu_publish(&batch[i]);
    return n;
}

int64_t __cajeta_prof_gpu_pending_overflow(void)  { return g_gpu_pending_overflow; }
int64_t __cajeta_prof_gpu_pending_unclaimed(void) { return g_gpu_pending_unclaimed; }

int32_t __cajeta_prof_gpu_pending_count(void) {
    int i, n = 0;
    pthread_mutex_lock(&g_gpu_pending_lock);
    for (i = 0; i < CAJ_GPU_PENDING_MAX; ++i) if (g_gpu_pending[i].in_use) n++;
    pthread_mutex_unlock(&g_gpu_pending_lock);
    return n;
}

void __cajeta_prof_gpu_pending_reset(void) {
    int i;
    pthread_mutex_lock(&g_gpu_pending_lock);
    for (i = 0; i < CAJ_GPU_PENDING_MAX; ++i) g_gpu_pending[i].in_use = 0;
    g_gpu_pending_overflow  = 0;
    g_gpu_pending_unclaimed = 0;
    pthread_mutex_unlock(&g_gpu_pending_lock);
}

static int32_t caj_gpu_rocm_begin(CajetaGpuEvent* ev) {
    // The host window is filled in either way: it is what gets reported if the
    // device record never comes back, and §10.4 says a degraded measurement
    // beats none. TIER_DEVICE is claimed only in resolve_dispatch.
    ev->dev_start_ns = __cajeta_currentTimeNanos();
    ev->tier = CAJETA_PROF_TIER_HOST;
    __cajeta_prof_rocm_push(ev->launch_id);
    return 1;
}

// Returns 0 to tell the seam "I have taken this one" — see the publish contract
// on __cajeta_prof_gpu_launch.
static int32_t caj_gpu_rocm_end(CajetaGpuEvent* ev) {
    ev->dev_end_ns = __cajeta_currentTimeNanos();
    __cajeta_prof_rocm_pop();
    if (!__cajeta_prof_rocm_tracing()) return 1;   // no records will come; publish now
    return caj_gpu_park(ev) ? 0 : 1;
}

static int32_t caj_gpu_rocm_collect(void) {
    __cajeta_prof_rocm_flush();
    // Whatever the flush did not claim has waited long enough. Its host window
    // is still true, so it goes out at host tier rather than being held for a
    // record that may never arrive.
    return caj_gpu_drain_pending();
}
static int32_t caj_gpu_rocm_calibrate(void) { return 1; }

static const CajetaGpuBackendVtbl caj_gpu_rocm_vtbl = {
    "rocm", caj_gpu_rocm_init, caj_gpu_rocm_begin, caj_gpu_rocm_end,
    caj_gpu_rocm_collect, caj_gpu_rocm_calibrate
};

// Backends that have not landed yet (NVIDIA is Unit 12, Vulkan Unit 13)
// degrade to host submit-to-complete rather than to nothing (§5.1.4). The tier
// says so, so a consumer can weight it (§5.6.6).
static const CajetaGpuBackendVtbl* caj_gpu_vtbl_for(int32_t backend) {
    if (backend == CAJ_GPU_BACKEND_HIP
            && __cajeta_prof_rocm_state() == CAJETA_ROCM_READY)
        return &caj_gpu_rocm_vtbl;
    return &caj_gpu_cpu_vtbl;
}

// What a given backend id actually resolved to, and at which tier. Both read
// the SAME selector the launch path uses, rather than re-deriving it — a test
// that asked a second implementation of the rule would pass while the launch
// path did something else.
const char* __cajeta_prof_gpu_backend_name(int32_t backend) {
    const CajetaGpuBackendVtbl* v = caj_gpu_vtbl_for(backend);
    return v && v->name ? v->name : "";
}

// Drain a backend's buffered device records and publish what they claim. The
// trace-end path calls it; so does anything that wants the device's answer
// before the run is over. Backends with nothing buffered answer 0.
int32_t __cajeta_prof_gpu_collect(int32_t backend) {
    const CajetaGpuBackendVtbl* v = caj_gpu_vtbl_for(backend);
    return (v && v->collect) ? v->collect() : 0;
}

int32_t __cajeta_prof_gpu_backend_tier(int32_t backend) {
    CajetaGpuEvent probe;
    const CajetaGpuBackendVtbl* v = caj_gpu_vtbl_for(backend);
    memset(&probe, 0, sizeof(probe));
    probe.tier = CAJETA_PROF_TIER_HOST;
    if (v && v->begin_launch) v->begin_launch(&probe);
    return probe.tier;
}

// ── the seam ──────────────────────────────────────────────────────────────

// The innermost shadow frame — where the program was when it launched (§5.1.2).
// Reads the same per-fiber shadow stack the sampler reads, so a launch and a
// sample never disagree about the call site, and a fiber-launched kernel is
// attributed to the fiber rather than to its carrier.
static void caj_gpu_call_site(CajetaGpuEvent* ev) {
    CajetaShadowFrame f;
    if (__cajeta_shadow_snapshot(&f, 1) == 1) {
        ev->call_site = f.desc;
        ev->call_site_line = f.line;
    }
}

// The one place a dispatch becomes a record. `run` is the actual backend
// dispatch; taking it as a thunk is what lets the launch chokepoint and the
// tests drive IDENTICAL code, rather than the tests re-deriving the bookkeeping
// against a seam that might never have been wired in.
void __cajeta_prof_gpu_launch(const char* kernelName,
                              int32_t gridX, int32_t gridY, int32_t gridZ,
                              int32_t blockX, int32_t blockY, int32_t blockZ,
                              uint32_t sharedBytes, int64_t streamHandle,
                              int32_t deviceId, int32_t backend,
                              void (*run)(void*), void* runArg) {
    // 7.1.e: unarmed is one acquire load and a branch, and the kernel still
    // runs. Nothing is minted, nothing is stamped, nothing is stacked.
    if (__atomic_load_n(&g_gpu_sinks_live, __ATOMIC_ACQUIRE) == 0) {
        if (run) run(runArg);
        return;
    }
    CajetaGpuEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.launch_id = __atomic_add_fetch(&g_gpu_launch_id, 1, __ATOMIC_ACQ_REL);
    ev.kernel_name = kernelName;
    ev.backend = backend;
    ev.device_id = deviceId < 0 ? 0 : deviceId;
    ev.queue = streamHandle;
    ev.host_thread = (void*) (uintptr_t) pthread_self();
    ev.grid_x = gridX;  ev.grid_y = gridY;  ev.grid_z = gridZ;
    ev.block_x = blockX; ev.block_y = blockY; ev.block_z = blockZ;
    ev.shared_bytes = sharedBytes;
    caj_gpu_call_site(&ev);

    const CajetaGpuBackendVtbl* vt = caj_gpu_vtbl_for(backend);
    ev.host_launch_ns = __cajeta_currentTimeNanos();
    if (vt->begin_launch) vt->begin_launch(&ev);
    if (run) run(runArg);
    // Stamped BEFORE end_launch so a backend that parks the event parks a
    // complete one. It also keeps end_launch's own cost out of the window,
    // which is the profiler's time and not the program's.
    ev.host_return_ns = __cajeta_currentTimeNanos();
    // end_launch returns 0 to say "I have taken this record and will publish it
    // myself, later" — the ROCm backend does that while it waits for the device
    // dispatch record that turns a host window into a device span. Any other
    // answer, including no end_launch at all, publishes here.
    if (vt->end_launch && vt->end_launch(&ev) == 0) return;
    __cajeta_prof_gpu_publish(&ev);
}

// ── the writer, as a sink ─────────────────────────────────────────────────
//
// The Perfetto writer registers like any other consumer. That is plan 7.3.b in
// executable form: if the writer needed a privileged path, "adding a consumer
// requires no change to any backend" would be a claim with one worked example
// and no second.
static CajProfWriter g_gpu_writer;
static CajGpuTracks  g_gpu_writer_tracks;
static int32_t       g_gpu_writer_sink = -1;
static int32_t       g_gpu_writer_open = 0;

static int32_t caj_gpu_writer_sink(const CajetaGpuEvent* recs, int32_t n, void* user) {
    (void) user;
    if (!g_gpu_writer_open) return 0;
    __cajeta_prof_gpu_emit(&g_gpu_writer, &g_gpu_writer_tracks, recs, n);
    return 0;
}

// ── 6.6 — capture, so a profiled run actually records GPU work ───────────
//
// Until this existed, CAJETA_PROFILER=1 armed the SAMPLER only. Nothing
// registered a GPU sink, so a profiled run of a GPU program produced a
// CPU-sampled trace with no device track, and Units 7 and 8 were reachable only
// from tests that attached a writer by hand. `__cajeta_prof_shutdown` already
// detached an attached GPU trace, which is how thoroughly the plumbing assumed
// something on the other end.
//
// The events go into the SAME file as the samples, not a second one. §8.3 wants
// host, fiber and device on one time axis and §8.8 wants that to be one file in
// Perfetto with no export step — and §3.4 already settled the identical
// question for instrumentation: "two files would make 'which tier produced this
// number' a question about provenance the reader has to keep track of by hand."
//
// Which means buffering until drain. The ring is bounded and drops the OLDEST
// on overflow, counting what it dropped — the same bargain the sampler's ring
// makes, for the same reason: a profiler that grows without limit changes the
// program it is measuring, and one that blocks changes it more.
#define CAJ_GPU_CAPTURE_DEFAULT 8192

static CajetaGpuEvent*  g_gpu_cap_ring = NULL;
static int32_t          g_gpu_cap_size = 0;
static int64_t          g_gpu_cap_head = 0;   // total ever written
static int64_t          g_gpu_cap_dropped = 0;
static int32_t          g_gpu_cap_sink = -1;
static pthread_mutex_t  g_gpu_cap_lock = PTHREAD_MUTEX_INITIALIZER;

static int32_t caj_gpu_capture_sink(const CajetaGpuEvent* recs, int32_t n, void* user) {
    (void) user;
    pthread_mutex_lock(&g_gpu_cap_lock);
    if (g_gpu_cap_ring && g_gpu_cap_size > 0) {
        for (int32_t i = 0; i < n; i++) {
            if (g_gpu_cap_head >= g_gpu_cap_size) g_gpu_cap_dropped++;
            g_gpu_cap_ring[g_gpu_cap_head % g_gpu_cap_size] = recs[i];
            g_gpu_cap_head++;
        }
    }
    pthread_mutex_unlock(&g_gpu_cap_lock);
    return 0;
}

// Armed from the same environment that arms the sampler, and early — §9.6 wants
// arming before any backend initializes, which is what lets Unit 8's
// rocprofiler configure hook find its window (it gates on
// __cajeta_prof_gpu_is_armed(), so before this the hook never fired outside
// tests).
int32_t __cajeta_prof_gpu_capture_arm(int32_t cap) {
    if (g_gpu_cap_sink >= 0) return -1;
    if (cap <= 0) cap = CAJ_GPU_CAPTURE_DEFAULT;
    pthread_mutex_lock(&g_gpu_cap_lock);
    g_gpu_cap_ring = (CajetaGpuEvent*) calloc((size_t) cap, sizeof(CajetaGpuEvent));
    g_gpu_cap_size = g_gpu_cap_ring ? cap : 0;
    g_gpu_cap_head = 0;
    g_gpu_cap_dropped = 0;
    pthread_mutex_unlock(&g_gpu_cap_lock);
    if (!g_gpu_cap_ring) return -3;
    // BATCHED: nothing is waiting on promptness, and per-record delivery would
    // wake the delivery thread for every launch.
    g_gpu_cap_sink = __cajeta_prof_gpu_sink_register(caj_gpu_capture_sink, NULL,
                                                     CAJETA_GPU_SINK_BATCHED);
    if (g_gpu_cap_sink < 0) { free(g_gpu_cap_ring); g_gpu_cap_ring = NULL; g_gpu_cap_size = 0; return -4; }
    return 0;
}

void __cajeta_prof_gpu_capture_disarm(void) {
    if (g_gpu_cap_sink >= 0) {
        __cajeta_prof_gpu_sink_unregister(g_gpu_cap_sink);
        g_gpu_cap_sink = -1;
    }
    pthread_mutex_lock(&g_gpu_cap_lock);
    free(g_gpu_cap_ring);
    g_gpu_cap_ring = NULL;
    g_gpu_cap_size = 0;
    g_gpu_cap_head = 0;
    g_gpu_cap_dropped = 0;
    pthread_mutex_unlock(&g_gpu_cap_lock);
}

int64_t __cajeta_prof_gpu_captured(void) {
    const int64_t head = __atomic_load_n(&g_gpu_cap_head, __ATOMIC_ACQUIRE);
    return head < (int64_t) g_gpu_cap_size ? head : (int64_t) g_gpu_cap_size;
}

int64_t __cajeta_prof_gpu_capture_dropped(void) {
    return __atomic_load_n(&g_gpu_cap_dropped, __ATOMIC_ACQUIRE);
}

/**
 * Emit what was captured into an already-open writer, in launch order.
 *
 * Called from the sampler's drain so both halves land in one file. Collects the
 * device's outstanding records first: a launch parked waiting for its dispatch
 * record is still parked at exit, and without this its device span would be
 * dropped in favour of the host window it already had.
 */
void __cajeta_prof_gpu_capture_settle(void) {
    // Claim the device's outstanding records and deliver everything queued.
    // Separate from the emit, and called BEFORE the trace's metadata packet is
    // written, because that packet carries the backend's account of itself
    // (§5.2.2) — settling afterwards produced traces annotated rocm_records=0
    // while 144 device spans sat in the same file.
    __cajeta_prof_gpu_collect(CAJ_GPU_BACKEND_HIP);
    __cajeta_prof_gpu_flush();
}

int64_t __cajeta_prof_gpu_captured_to_trace(CajProfWriter* w, uint64_t ts) {
    (void) ts;
    __cajeta_prof_gpu_capture_settle();

    pthread_mutex_lock(&g_gpu_cap_lock);
    const int64_t head = g_gpu_cap_head;
    const int32_t size = g_gpu_cap_size;
    if (!g_gpu_cap_ring || size <= 0 || head <= 0) {
        pthread_mutex_unlock(&g_gpu_cap_lock);
        return 0;
    }
    int64_t n = head < (int64_t) size ? head : (int64_t) size;
    CajetaGpuEvent* ordered = (CajetaGpuEvent*) malloc((size_t) n * sizeof(CajetaGpuEvent));
    if (!ordered) { pthread_mutex_unlock(&g_gpu_cap_lock); return 0; }
    const int64_t tail = head - n;
    for (int64_t i = 0; i < n; i++) ordered[i] = g_gpu_cap_ring[(tail + i) % size];
    pthread_mutex_unlock(&g_gpu_cap_lock);

    CajGpuTracks seen;
    seen.n = 0;
    int64_t packets = __cajeta_prof_gpu_emit(w, &seen, ordered, (int32_t) n);
    free(ordered);
    return packets;
}

// A run that dispatched to the GPU but collected no samples still measured
// something. Mirrors __cajeta_prof_instr_only_to_trace, and exists for the same
// reason: the sampler's drain returns early on an empty ring, so without this a
// short GPU program profiles to an empty file.
int64_t __cajeta_prof_gpu_only_to_trace(const char* path) {
    if (__cajeta_prof_gpu_captured() <= 0 && __cajeta_prof_gpu_pending_count() <= 0) return 0;
    static CajProfWriter w;
    if (!__cajeta_prof_trace_open(&w, path)) return 0;
    __cajeta_prof_trace_metadata(&w, 0, "gpu", 0, 0, 0, 0, 0);
    __cajeta_prof_gpu_captured_to_trace(&w, 0);
    int64_t packets = __cajeta_prof_trace_packets(&w);
    __cajeta_prof_trace_close(&w);
    return packets;
}

int32_t __cajeta_prof_gpu_trace_attach(const char* path) {
    if (g_gpu_writer_open) return -1;
    if (!__cajeta_prof_trace_open(&g_gpu_writer, path)) return -2;
    g_gpu_writer_tracks.n = 0;
    g_gpu_writer_open = 1;
    g_gpu_writer_sink = __cajeta_prof_gpu_sink_register(
        caj_gpu_writer_sink, NULL, CAJETA_GPU_SINK_BATCHED);
    if (g_gpu_writer_sink < 0) {
        __cajeta_prof_trace_close(&g_gpu_writer);
        g_gpu_writer_open = 0;
        return -3;
    }
    return 0;
}

int32_t __cajeta_prof_gpu_trace_detach(void) {
    if (!g_gpu_writer_open) return 0;
    // Claim what the device has finished, THEN publish whatever is still
    // parked at host tier, THEN deliver. A launch waiting on a record that
    // never came back is still a measurement, and dropping it at trace end
    // would leave a hole where a kernel plainly ran.
    __cajeta_prof_gpu_collect(CAJ_GPU_BACKEND_HIP);
    __cajeta_prof_gpu_flush();     // whatever is queued belongs in the file
    // §7.8's run metadata, and with it the backend's own account of itself
    // (§5.2.2). A GPU-only trace was previously written with no metadata packet
    // at all, so a reader opening one had no way to tell a device-timed run
    // from a degraded one — the two render identically. Written at detach
    // rather than attach because the counters it carries are only final here.
    __cajeta_prof_trace_metadata(&g_gpu_writer, 0, "gpu", 0, 0, 0, 0, 0);
    if (g_gpu_writer_sink >= 0) __cajeta_prof_gpu_sink_unregister(g_gpu_writer_sink);
    g_gpu_writer_sink = -1;
    g_gpu_writer_open = 0;
    __cajeta_prof_trace_close(&g_gpu_writer);
    return 1;
}
