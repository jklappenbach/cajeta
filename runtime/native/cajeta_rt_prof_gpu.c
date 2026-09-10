// === The GPU dispatch record seam. #included into cajeta_runtime.c AFTER
// === cajeta_rt_prof_trace.c and BEFORE cajeta_xpu.c. Publication to sinks is
// === bounded and never waits; each sink gets its own copy in its own queue.

#include <pthread.h>

int64_t __cajeta_currentTimeNanos(void);

// ── per-sink queue ────────────────────────────────────────────────────────
// Vyukov-style bounded MPMC ring: the per-slot sequence separates a producer's
// claim from its write, so a claim in flight is invisible to the consumer.
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
// Held while a sink is CALLED: a flush and the delivery thread must not enter
// the same sink at once. Producers never take it.
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

// Allocate one sink's ring, rounded up to a power of two. 0 if calloc failed.
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

// Never blocks: 1 if the record was queued, 0 if the queue was full (a drop).
static int32_t caj_gpu_enqueue(CajGpuSink* s, const CajetaGpuEvent* ev) {
    int64_t pos = __atomic_load_n(&s->head, __ATOMIC_RELAXED);
    for (;;) {
        CajGpuSlot* slot = &s->ring[pos & s->mask];
        int64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        int64_t d = seq - pos;
        if (d == 0) {
            if (__atomic_compare_exchange_n(&s->head, &pos, pos + 1, 1,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                slot->ev = *ev;      // each sink's OWN copy
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

// Take one record for the single consumer; 0 when empty or still being written.
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

// Drain one sink into its callback under the delivery lock; returns the count.
static int32_t caj_gpu_drain_one(CajGpuSink* s) {
    if (!s->in_use || !s->enabled || !s->fn) return 0;
    CajetaGpuEvent batch[CAJ_GPU_BATCH_MAX];
    int32_t total = 0;
    for (;;) {
        int32_t n = 0;
        while (n < CAJ_GPU_BATCH_MAX && caj_gpu_dequeue(s, &batch[n])) {
            n++;
            if (s->granularity == CAJETA_GPU_SINK_PER_RECORD) break;
        }
        if (n == 0) return total;
        int32_t rc = s->fn(batch, n, s->user);
        if (rc != 0) {
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

// Deliver everything queued, on the caller's thread. Returns the sinks touched.
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

// The delivery thread: per-record sinks drain promptly, batched ones half full.
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

// Register a record consumer and ARM the seam, starting the delivery thread on
// the first sink. Anything not PER_RECORD is batched. Returns the id, or -1.
int32_t __cajeta_prof_gpu_sink_register(CajetaGpuSinkFn fn, void* user,
                                        int32_t granularity) {
    if (!fn) return -1;
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
    __atomic_add_fetch(&g_gpu_sinks_live, 1, __ATOMIC_RELEASE);
    if (!g_gpu_deliver_running) {
        g_gpu_deliver_stop = 0;
        if (pthread_create(&g_gpu_deliver_thread, NULL, caj_gpu_deliver_loop, NULL) == 0)
            g_gpu_deliver_running = 1;
    }
    pthread_mutex_unlock(&g_gpu_reg_lock);
    return id;
}

// Disarm and free one sink, joining the delivery thread with the last of them.
int32_t __cajeta_prof_gpu_sink_unregister(int32_t id) {
    if (id < 0 || id >= CAJETA_GPU_MAX_SINKS) return 0;
    pthread_mutex_lock(&g_gpu_reg_lock);
    CajGpuSink* s = &g_gpu_sink[id];
    if (!s->in_use) { pthread_mutex_unlock(&g_gpu_reg_lock); return 0; }
// Stop publication first: a sink must not be freed while delivery is inside it.
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
// Queue capacity for sinks registered AFTER this call. Returns what is in force.
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

// CPU emulation: host wall time IS a synchronous dispatch's span, at TIER_HOST.
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
// Nothing is buffered and no device clock: present to keep the vtable uniform.
static int32_t caj_gpu_cpu_collect(void)   { return 0; }
static int32_t caj_gpu_cpu_calibrate(void) { return 1; }

static const CajetaGpuBackendVtbl caj_gpu_cpu_vtbl = {
    "cpu", caj_gpu_cpu_init, caj_gpu_cpu_begin, caj_gpu_cpu_end,
    caj_gpu_cpu_collect, caj_gpu_cpu_calibrate
};

// ROCm. Selected only once rocprofiler-sdk is bound; until then the backend
// takes the host lane, since a zero device span would look like a measured one.
static int32_t caj_gpu_rocm_init(void) { return __cajeta_prof_rocm_init(); }

// ── the pending table ─────────────────────────────────────────────────────
// A dispatch record arrives after the launch that caused it returned, so a
// launch awaiting its span PARKS here; full publishes at host tier instead.
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

// Park a launch awaiting its record. 1 = parked (do NOT publish), 0 = full.
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

// Resolve a parked launch with the span its backend supplied. `tier` is the
// CALLER's claim: vendor records are TIER_DEVICE, query brackets TIER_EVENT.
int32_t __cajeta_prof_gpu_resolve_dispatch_flags(int64_t launchId,
                                                 int64_t devStartNs,
                                                 int64_t devEndNs,
                                                 int32_t tier,
                                                 int32_t integrityFlags) {
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
    ev.tier = tier;
    ev.integrity_flags |= integrityFlags;
// dev_end is bounded by resolved_ns, not host_return_ns, which async overruns.
    ev.resolved_ns = __cajeta_currentTimeNanos();
    __cajeta_prof_gpu_publish(&ev);   // outside the lock: a sink runs user code
    return 1;
}

// resolve_dispatch_flags with no additional integrity flags.
int32_t __cajeta_prof_gpu_resolve_dispatch_tier(int64_t launchId,
                                                int64_t devStartNs,
                                                int64_t devEndNs,
                                                int32_t tier) {
    return __cajeta_prof_gpu_resolve_dispatch_flags(launchId, devStartNs,
                                                    devEndNs, tier,
                                                    CAJETA_SPAN_OK);
}

// Resolve at TIER_DEVICE: a vendor dispatch record supplied the span.
int32_t __cajeta_prof_gpu_resolve_dispatch(int64_t launchId,
                                           int64_t devStartNs, int64_t devEndNs) {
    return __cajeta_prof_gpu_resolve_dispatch_tier(launchId, devStartNs,
                                                   devEndNs,
                                                   CAJETA_PROF_TIER_DEVICE);
}

// Publish everything still parked, at host tier; returns how many went out.
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

// Stamp the host window and push the launch id. The window is what gets
// reported if the record never comes; TIER_DEVICE is claimed only on resolve.
static int32_t caj_gpu_rocm_begin(CajetaGpuEvent* ev) {
    ev->dev_start_ns = __cajeta_currentTimeNanos();
    ev->tier = CAJETA_PROF_TIER_HOST;
    __cajeta_prof_rocm_push(ev->launch_id);
    return 1;
}

// Returns 0 to tell the seam it has taken this one; see __cajeta_prof_gpu_launch.
static int32_t caj_gpu_rocm_end(CajetaGpuEvent* ev) {
    ev->dev_end_ns = __cajeta_currentTimeNanos();
    __cajeta_prof_rocm_pop();
    if (!__cajeta_prof_rocm_tracing()) return 1;   // no records will come; publish now
    return caj_gpu_park(ev) ? 0 : 1;
}

// Flush the vendor's records; whatever it did not claim goes out at host tier.
static int32_t caj_gpu_rocm_collect(void) {
    __cajeta_prof_rocm_flush();
    return caj_gpu_drain_pending();
}
static int32_t caj_gpu_rocm_calibrate(void) { return 1; }

static const CajetaGpuBackendVtbl caj_gpu_rocm_vtbl = {
    "rocm", caj_gpu_rocm_init, caj_gpu_rocm_begin, caj_gpu_rocm_end,
    caj_gpu_rocm_collect, caj_gpu_rocm_calibrate
};

// ── NVIDIA / CUPTI ────────────────────────────────────────────────────────
// Structurally the ROCm backend: the launch id is CUPTI's external correlation
// id; below it, the event-tier fallback state for drivers that have no CUPTI.
static int32_t caj_gpu_cuda_events_armed;
static char    caj_gpu_cuda_events_why[256];
static int64_t caj_gpu_cuda_event_spans;

// Record whether the driver's event lane armed, and the reason when it did not.
void __cajeta_prof_cuda_events_note(int32_t ok, const char* why) {
    caj_gpu_cuda_events_armed = ok ? 1 : 0;
    if (why) snprintf(caj_gpu_cuda_events_why, sizeof(caj_gpu_cuda_events_why),
                      "%s", why);
}
int32_t     __cajeta_prof_cuda_events_ok(void)     { return caj_gpu_cuda_events_armed; }
const char* __cajeta_prof_cuda_events_reason(void) { return caj_gpu_cuda_events_why; }
int64_t     __cajeta_prof_cuda_event_spans(void)   { return caj_gpu_cuda_event_spans; }

// The host instant a still-parked launch was issued at, or 0 if not parked.
static int64_t caj_gpu_pending_host_launch(int64_t launchId) {
    int64_t v = 0;
    int i;
    pthread_mutex_lock(&g_gpu_pending_lock);
    for (i = 0; i < CAJ_GPU_PENDING_MAX; ++i) {
        if (!g_gpu_pending[i].in_use || g_gpu_pending[i].launch_id != launchId)
            continue;
        v = g_gpu_pending[i].ev.host_launch_ns;
        break;
    }
    pthread_mutex_unlock(&g_gpu_pending_lock);
    return v;
}

// The launch the dispatcher should bracket. Thread-local: two threads may be
// launching at once, and a bracket belongs to exactly one of them.
static __thread int64_t caj_gpu_cuda_current_launch;
int64_t __cajeta_prof_cuda_current_launch(void) { return caj_gpu_cuda_current_launch; }

// A finished event bracket, at EVENT tier. Its duration is device-measured but
// its placement is not, so the span is translated to sit inside [launch, now].
void __cajeta_prof_cuda_bracket_resolved(int64_t launchId, int64_t devStartNs,
                                         int64_t devEndNs) {
// Bounded by the dispersion cap: an excursion it cannot explain is a real shear.
    const int64_t lo    = caj_gpu_pending_host_launch(launchId);
    const int64_t hi    = __cajeta_currentTimeNanos();
    const int64_t slack = __cajeta_prof_clock_dispersion_cap();
    if (lo > 0 && devEndNs - devStartNs <= hi - lo) {
        int64_t shift = 0;
        if (devStartNs < lo)     shift = lo - devStartNs;
        else if (devEndNs > hi)  shift = hi - devEndNs;
        if (shift != 0 && shift <= slack && shift >= -slack
                && devStartNs + shift >= lo && devEndNs + shift <= hi) {
            devStartNs += shift;
            devEndNs   += shift;
        }
    }
    if (__cajeta_prof_gpu_resolve_dispatch_tier(launchId, devStartNs, devEndNs,
                                                CAJETA_PROF_TIER_EVENT))
        caj_gpu_cuda_event_spans++;
}

// CUPTI when it is tracing, the driver's own events otherwise.
static int32_t caj_gpu_cuda_init(void) {
    return __cajeta_prof_cupti_tracing() || caj_gpu_cuda_events_armed;
}

// Stamp the host window and push the launch id as CUPTI's correlation id.
static int32_t caj_gpu_cuda_begin(CajetaGpuEvent* ev) {
    ev->dev_start_ns = __cajeta_currentTimeNanos();
    ev->tier = CAJETA_PROF_TIER_HOST;
    __cajeta_prof_cupti_push(ev->launch_id);
    // The tier ladder is a precedence order: while CUPTI is tracing, a better
    // DEVICE-tier record is coming, and publishing the launch id too would let
    // the event bracket race it for the same parked launch and downgrade it.
    caj_gpu_cuda_current_launch =
        (caj_gpu_cuda_events_armed && !__cajeta_prof_cupti_tracing())
            ? ev->launch_id : 0;
    return 1;
}

// Returns 0 for "I have taken this one" — __cajeta_prof_gpu_launch's contract.
static int32_t caj_gpu_cuda_end(CajetaGpuEvent* ev) {
    ev->dev_end_ns = __cajeta_currentTimeNanos();
    __cajeta_prof_cupti_pop();
    caj_gpu_cuda_current_launch = 0;
    if (!__cajeta_prof_cupti_tracing() && !caj_gpu_cuda_events_armed)
        return 1;   // no record and no bracket will come
    return caj_gpu_park(ev) ? 0 : 1;
}

// Flush CUPTI's records; whatever it did not claim goes out at host tier.
static int32_t caj_gpu_cuda_collect(void) {
    __cajeta_prof_cupti_flush();
    return caj_gpu_drain_pending();
}

// Records arrive in the host domain already, stamped by the timestamp callback.
static int32_t caj_gpu_cuda_calibrate(void) { return 1; }

static const CajetaGpuBackendVtbl caj_gpu_cuda_vtbl = {
    "cuda", caj_gpu_cuda_init, caj_gpu_cuda_begin, caj_gpu_cuda_end,
    caj_gpu_cuda_collect, caj_gpu_cuda_calibrate
};

// ── Vulkan ────────────────────────────────────────────────────────────────
// The mechanics live in cajeta_xpu_vulkan.c; the seam contract lives here: the
// launch parks, and the dispatcher resolves it at EVENT tier from the same thread.
static __thread int64_t caj_gpu_vk_current_launch;

int64_t __cajeta_prof_vk_current_launch(void) { return caj_gpu_vk_current_launch; }

static int32_t caj_gpu_vk_init(void) { return __cajeta_prof_vk_timing_ok(); }

// Stamp the host window; it is what is reported if the bracket never resolves.
static int32_t caj_gpu_vk_begin(CajetaGpuEvent* ev) {
    ev->dev_start_ns = __cajeta_currentTimeNanos();
    ev->tier = CAJETA_PROF_TIER_HOST;
    caj_gpu_vk_current_launch = ev->launch_id;
    return 1;
}

// The bracket lands while the launch is still inside __cajeta_prof_gpu_launch,
// before the park, so it waits here — one slot, TLS — until end_launch applies it.
static __thread struct {
    int64_t launch_id;
    int64_t start_ns;
    int64_t end_ns;
    int32_t flags;      // producer-only integrity flags
    int32_t valid;
} caj_gpu_vk_bracket;

// Hand a finished query-pool bracket to the launch running on this thread.
void __cajeta_prof_vk_bracket_resolved(int64_t launchId, int64_t devStartNs,
                                       int64_t devEndNs, int32_t flags) {
    caj_gpu_vk_bracket.launch_id = launchId;
    caj_gpu_vk_bracket.start_ns = devStartNs;
    caj_gpu_vk_bracket.end_ns = devEndNs;
    caj_gpu_vk_bracket.flags = flags;
    caj_gpu_vk_bracket.valid = 1;
}

// Returns 0 for "I have taken this one" — __cajeta_prof_gpu_launch's contract.
static int32_t caj_gpu_vk_end(CajetaGpuEvent* ev) {
    ev->dev_end_ns = __cajeta_currentTimeNanos();
    caj_gpu_vk_current_launch = 0;
    if (!__cajeta_prof_vk_timing_ok()) {
        caj_gpu_vk_bracket.valid = 0;   // nothing should have been left
        return 1;                       // no bracket ran; publish now
    }
    if (!caj_gpu_park(ev)) {
        caj_gpu_vk_bracket.valid = 0;   // table full: host window goes out as-is
        return 1;
    }
// Park FIRST: resolution goes through the pending table, so it must be in it.
    if (caj_gpu_vk_bracket.valid) {
        caj_gpu_vk_bracket.valid = 0;
        int64_t s = caj_gpu_vk_bracket.start_ns;
        int64_t e = caj_gpu_vk_bracket.end_ns;
// Placement through the fitted clock mapping carries the fit's uncertainty; the
// dispatch is synchronous, so translate into [submit, now], bounded by the cap.
        {
            const int64_t lo = ev->host_launch_ns;
            const int64_t hi = __cajeta_currentTimeNanos();
            const int64_t slack = __cajeta_prof_clock_dispersion_cap();
            if (e - s <= hi - lo) {
                int64_t shift = 0;
                if (s < lo)      shift = lo - s;
                else if (e > hi) shift = hi - e;
                if (shift != 0 && shift <= slack && shift >= -slack
                        && s + shift >= lo && e + shift <= hi) {
                    s += shift;
                    e += shift;
                }
            }
        }
        if (__cajeta_prof_gpu_resolve_dispatch_flags(caj_gpu_vk_bracket.launch_id,
                                                     s, e,
                                                     CAJETA_PROF_TIER_EVENT,
                                                     caj_gpu_vk_bracket.flags))
            __cajeta_prof_vk_note_resolved();
    }
    return 0;
}

// Nothing buffers: anything still parked has no bracket coming and goes out host.
static int32_t caj_gpu_vk_collect(void) {
    return caj_gpu_drain_pending();
}
static int32_t caj_gpu_vk_calibrate(void) { return 1; }

static const CajetaGpuBackendVtbl caj_gpu_vk_vtbl = {
    "vulkan", caj_gpu_vk_init, caj_gpu_vk_begin, caj_gpu_vk_end,
    caj_gpu_vk_collect, caj_gpu_vk_calibrate
};

// The host blocked on vkQueueWaitIdle, published as its own host-tier span so a
// reader sees it rather than an absence. 1 when published, 0 when unarmed.
int32_t __cajeta_prof_vk_note_wait(int64_t queue, int64_t startNs,
                                   int64_t endNs) {
    if (__atomic_load_n(&g_gpu_sinks_live, __ATOMIC_ACQUIRE) == 0) return 0;
    CajetaGpuEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.launch_id = __atomic_add_fetch(&g_gpu_launch_id, 1, __ATOMIC_ACQ_REL);
    ev.kernel_name = "host blocked on GPU";
    ev.backend = CAJ_GPU_BACKEND_VULKAN;
    ev.queue = queue;
    ev.host_thread = (void*) (uintptr_t) pthread_self();
    ev.host_launch_ns = startNs;
    ev.host_return_ns = endNs;
    ev.dev_start_ns = startNs;   // a host-measured wall interval: host tier,
    ev.dev_end_ns = endNs;       // and the "device" span IS the host window
    ev.tier = CAJETA_PROF_TIER_HOST;
    __cajeta_prof_gpu_publish(&ev);
    return 1;
}

// The vtable a backend id resolves to; an unusable SDK degrades to the host lane.
static const CajetaGpuBackendVtbl* caj_gpu_vtbl_for(int32_t backend) {
    if (backend == CAJ_GPU_BACKEND_HIP
            && __cajeta_prof_rocm_state() == CAJETA_ROCM_READY)
        return &caj_gpu_rocm_vtbl;
// tracing(), not state(): a CUPTI with no activity kind enabled produces no
// records, so parking would hold a launch forever. Events are the fallback.
    if (backend == CAJ_GPU_BACKEND_CUDA
            && (__cajeta_prof_cupti_tracing() || caj_gpu_cuda_events_armed))
        return &caj_gpu_cuda_vtbl;
    if (backend == CAJ_GPU_BACKEND_VULKAN && __cajeta_prof_vk_timing_ok())
        return &caj_gpu_vk_vtbl;
    return &caj_gpu_cpu_vtbl;
}

// The name a backend id resolves to, through the SAME selector the launch uses.
const char* __cajeta_prof_gpu_backend_name(int32_t backend) {
    const CajetaGpuBackendVtbl* v = caj_gpu_vtbl_for(backend);
    return v && v->name ? v->name : "";
}

// Drain a backend's buffered device records; backends with none answer 0.
int32_t __cajeta_prof_gpu_collect(int32_t backend) {
    const CajetaGpuBackendVtbl* v = caj_gpu_vtbl_for(backend);
    return (v && v->collect) ? v->collect() : 0;
}

// The tier a backend id would claim, probed through its own begin_launch.
int32_t __cajeta_prof_gpu_backend_tier(int32_t backend) {
    CajetaGpuEvent probe;
    const CajetaGpuBackendVtbl* v = caj_gpu_vtbl_for(backend);
    memset(&probe, 0, sizeof(probe));
    probe.tier = CAJETA_PROF_TIER_HOST;
    if (v && v->begin_launch) v->begin_launch(&probe);
    return probe.tier;
}

// ── the seam ──────────────────────────────────────────────────────────────

// The innermost shadow frame: the same per-fiber stack the sampler reads.
static void caj_gpu_call_site(CajetaGpuEvent* ev) {
    CajetaShadowFrame f;
    if (__cajeta_shadow_snapshot(&f, 1) == 1) {
        ev->call_site = f.desc;
        ev->call_site_line = f.line;
    }
}

// The one place a dispatch becomes a record. `run` is the actual backend
// dispatch, taken as a thunk so the launch chokepoint and the tests drive
// identical code. Unarmed, this is one acquire load, a branch, and run().
void __cajeta_prof_gpu_launch(const char* kernelName,
                              int32_t gridX, int32_t gridY, int32_t gridZ,
                              int32_t blockX, int32_t blockY, int32_t blockZ,
                              uint32_t sharedBytes, int64_t streamHandle,
                              int32_t deviceId, int32_t backend,
                              void (*run)(void*), void* runArg) {
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
// Stamped BEFORE end_launch, so a backend that parks the event parks a whole one.
    ev.host_return_ns = __cajeta_currentTimeNanos();
// end_launch returns 0 for "I have taken this record"; anything else publishes.
    if (vt->end_launch && vt->end_launch(&ev) == 0) return;
    __cajeta_prof_gpu_publish(&ev);
}

// ── the writer, as a sink ─────────────────────────────────────────────────
// The Perfetto writer registers like any other consumer, with no special path.
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

// ── capture ───────────────────────────────────────────────────────────────
// GPU events go into the SAME file as the samples, so they buffer here until
// the drain. Bounded, dropping the OLDEST and counting what it dropped.
#define CAJ_GPU_CAPTURE_DEFAULT 8192

static CajetaGpuEvent*  g_gpu_cap_ring = NULL;
static int32_t          g_gpu_cap_size = 0;
static int64_t          g_gpu_cap_head = 0;   // total ever written
static int64_t          g_gpu_cap_dropped = 0;
static int32_t          g_gpu_cap_sink = -1;
static pthread_mutex_t  g_gpu_cap_lock = PTHREAD_MUTEX_INITIALIZER;

// The capture sink: copy a delivered batch into the ring, oldest dropped first.
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

// Arm capture with a `cap`-record ring (0 = default). 0, -1 already armed, -3/-4
// ring or sink failed. Runs before any backend init, which gates on is_armed().
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
    g_gpu_cap_sink = __cajeta_prof_gpu_sink_register(caj_gpu_capture_sink, NULL,
                                                     CAJETA_GPU_SINK_BATCHED);
    if (g_gpu_cap_sink < 0) { free(g_gpu_cap_ring); g_gpu_cap_ring = NULL; g_gpu_cap_size = 0; return -4; }
    return 0;
}

// Unregister the capture sink and free the ring. Idempotent.
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

// Claim the device's outstanding records and deliver everything queued. Runs
// BEFORE the metadata packet, which carries the backend's account of itself.
void __cajeta_prof_gpu_capture_settle(void) {
    __cajeta_prof_gpu_collect(CAJ_GPU_BACKEND_HIP);
    __cajeta_prof_gpu_flush();
}

// Emit everything captured into an open writer, in launch order; settles first.
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

// Write a GPU-only trace to `path`; the sampler's drain skips an empty ring.
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

// Open `path` and register the writer as a batched sink. 0, or a negative code.
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

// Settle, write the run metadata, close the trace. Returns 1 if one was open.
int32_t __cajeta_prof_gpu_trace_detach(void) {
    if (!g_gpu_writer_open) return 0;
// Collect, THEN publish what is still parked at host tier, THEN deliver: a
// launch whose record never came back is still a measurement.
    __cajeta_prof_gpu_collect(CAJ_GPU_BACKEND_HIP);
    __cajeta_prof_gpu_flush();
// Written at detach, not attach: the counters it carries are final only here.
    __cajeta_prof_trace_metadata(&g_gpu_writer, 0, "gpu", 0, 0, 0, 0, 0);
    if (g_gpu_writer_sink >= 0) __cajeta_prof_gpu_sink_unregister(g_gpu_writer_sink);
    g_gpu_writer_sink = -1;
    g_gpu_writer_open = 0;
    __cajeta_prof_trace_close(&g_gpu_writer);
    return 1;
}
