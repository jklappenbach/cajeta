// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// Poison-on-free (CompilerModes.md § --poison-free): memsets every heap block to
// 0xDB just before free(), so a use-after-free traps or reads obviously wrong.
static int __cajeta_poison_free_enabled = 0;

void __cajeta_set_poison_free(int enabled) {
    __cajeta_poison_free_enabled = enabled ? 1 : 0;
}

int __cajeta_get_poison_free(void) {
    return __cajeta_poison_free_enabled;
}

// Sentinel-fill a buffer with 0xDB to its chunk size; no-op when off or NULL.
void __cajeta_poison_buffer(void* ptr) {
    if (!__cajeta_poison_free_enabled) return;
    if (!ptr) return;
    size_t n = cajeta_malloc_usable_size(ptr);
    if (n == 0) return;
    memset(ptr, 0xDB, n);
}

// Debug safepoints: under --debug-info, statement codegen calls this before each
// statement. The JIT'd and the native copy each keep their own counter.
static long __cajeta_dbg_safepoint_total = 0;

// Monotonic fiber-id source, bumped at fiber creation by __cajeta_task_run.
long __cajeta_dbg_fiber_id_counter = 0;

// ── Debugger live-fiber registry ──────────────────────────────────────────
// A fiber registers at __cajeta_task_run and unregisters when the carrier frees
// it. Its own mutex, locked one way only (task then reg, never the reverse).
static pthread_mutex_t __cajeta_dbg_fiber_reg_mutex = PTHREAD_MUTEX_INITIALIZER;
static void** __cajeta_dbg_fiber_reg = NULL;
static int __cajeta_dbg_fiber_reg_count = 0;
static int __cajeta_dbg_fiber_reg_cap = 0;

void __cajeta_dbg_fiber_register(void* fiber) {
    if (!fiber) return;
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    if (__cajeta_dbg_fiber_reg_count == __cajeta_dbg_fiber_reg_cap) {
        int cap = __cajeta_dbg_fiber_reg_cap ? __cajeta_dbg_fiber_reg_cap * 2 : 16;
        void** grown = realloc(__cajeta_dbg_fiber_reg, (size_t) cap * sizeof(void*));
        if (!grown) { pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex); return; }
        __cajeta_dbg_fiber_reg = grown;
        __cajeta_dbg_fiber_reg_cap = cap;
    }
    __cajeta_dbg_fiber_reg[__cajeta_dbg_fiber_reg_count++] = fiber;
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
}

void __cajeta_dbg_fiber_unregister(void* fiber) {
    if (!fiber) return;
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    for (int i = 0; i < __cajeta_dbg_fiber_reg_count; i++) {
        if (__cajeta_dbg_fiber_reg[i] != fiber) continue;
        // Order-preserving removal: the view stays in spawn order across stops.
        for (int j = i + 1; j < __cajeta_dbg_fiber_reg_count; j++) {
            __cajeta_dbg_fiber_reg[j - 1] = __cajeta_dbg_fiber_reg[j];
        }
        __cajeta_dbg_fiber_reg_count--;
        break;
    }
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
}

// Number of live fibers, excluding the program thread (a synthetic id-0 thread).
int __cajeta_dbg_fiber_count(void) {
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    int n = __cajeta_dbg_fiber_reg_count;
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
    return n;
}

// The index-th live fiber handle (spawn order), or NULL if out of range.
void* __cajeta_dbg_fiber_at(int index) {
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    void* f = (index >= 0 && index < __cajeta_dbg_fiber_reg_count)
                  ? __cajeta_dbg_fiber_reg[index]
                  : NULL;
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
    return f;
}

// Copy up to `max` live-fiber handles (spawn order) into `out` under a SINGLE
// lock hold and return the live count; count() then at(i) would be a TOCTOU.
int __cajeta_dbg_fiber_snapshot(void** out, int max) {
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    int n = __cajeta_dbg_fiber_reg_count;
    if (out && max > 0) {
        int copy = n < max ? n : max;
        for (int i = 0; i < copy; i++) out[i] = __cajeta_dbg_fiber_reg[i];
    }
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
    return n;
}

// Test-only: drop all registry entries (does NOT free the fibers themselves).
void __cajeta_dbg_fiber_reg_reset(void) {
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    __cajeta_dbg_fiber_reg_count = 0;
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
}

// === Debug-only stop coordinator ==========================================
// Process-global rendezvous for stop-the-world: a breakpoint sets the flag,
// carriers park at their next safepoint, the debugger converges and then clears.
static pthread_mutex_t __cajeta_stop_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __cajeta_stop_resume_cv    = PTHREAD_COND_INITIALIZER; // carriers wait here
static pthread_cond_t  __cajeta_stop_converged_cv = PTHREAD_COND_INITIALIZER; // debugger waits here
static int      __cajeta_stop_requested = 0;   // 0/1 flag — hot-path relaxed read
static unsigned __cajeta_stop_generation = 0;  // bumped on every request and clear
static int      __cajeta_stop_parked = 0;      // carriers currently parked
static int      __cajeta_stop_expected = 0;    // carriers that must park before inspect

// Hot path: a single relaxed load, gated upstream by the safepoint guard.
int __cajeta_stop_is_requested(void) {
    return __atomic_load_n(&__cajeta_stop_requested, __ATOMIC_RELAXED);
}

// Open a stop round. Returns 1 iff this caller flipped 0->1 (the primary).
int __cajeta_stop_request(void) {
    int primary = 0;
    pthread_mutex_lock(&__cajeta_stop_mu);
    if (!__atomic_load_n(&__cajeta_stop_requested, __ATOMIC_RELAXED)) {
        __atomic_store_n(&__cajeta_stop_requested, 1, __ATOMIC_RELAXED);
        __cajeta_stop_generation++;
        primary = 1;
    }
    pthread_mutex_unlock(&__cajeta_stop_mu);
    return primary;
}

// Resume-all: clear the flag, bump the generation, wake every parked carrier.
void __cajeta_stop_clear(void) {
    pthread_mutex_lock(&__cajeta_stop_mu);
    __atomic_store_n(&__cajeta_stop_requested, 0, __ATOMIC_RELAXED);
    __cajeta_stop_generation++;
    pthread_cond_broadcast(&__cajeta_stop_resume_cv);
    pthread_mutex_unlock(&__cajeta_stop_mu);
}

// How many carriers must park before the debugger treats the world as stopped.
void __cajeta_stop_set_expected(int n) {
    pthread_mutex_lock(&__cajeta_stop_mu);
    __cajeta_stop_expected = n;
    pthread_cond_broadcast(&__cajeta_stop_converged_cv);  // a lowered bar may already be met
    pthread_mutex_unlock(&__cajeta_stop_mu);
}

// A carrier that observed the stop parks here: count in, wake the convergence
// wait, block until THIS round clears (a cleared or newer round releases it).
void __cajeta_stop_park(void) {
    pthread_mutex_lock(&__cajeta_stop_mu);
    unsigned gen = __cajeta_stop_generation;
    __cajeta_stop_parked++;
    pthread_cond_broadcast(&__cajeta_stop_converged_cv);
    while (__atomic_load_n(&__cajeta_stop_requested, __ATOMIC_RELAXED)
           && __cajeta_stop_generation == gen) {
        pthread_cond_wait(&__cajeta_stop_resume_cv, &__cajeta_stop_mu);
    }
    __cajeta_stop_parked--;
    pthread_mutex_unlock(&__cajeta_stop_mu);
}

// Debugger-side barrier: block until parked >= expected or timeout_ns elapses
// (<= 0 waits indefinitely). Returns the count still NOT parked (0 == quiesced).
int __cajeta_stop_wait_converged(long timeout_ns) {
    struct timespec deadline;
    if (timeout_ns > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        long sec = timeout_ns / 1000000000L;
        long nsec = timeout_ns % 1000000000L;
        deadline.tv_sec += sec;
        deadline.tv_nsec += nsec;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }
    }
    pthread_mutex_lock(&__cajeta_stop_mu);
    while (__cajeta_stop_parked < __cajeta_stop_expected) {
        if (timeout_ns <= 0) {
            pthread_cond_wait(&__cajeta_stop_converged_cv, &__cajeta_stop_mu);
        } else if (pthread_cond_timedwait(&__cajeta_stop_converged_cv,
                                          &__cajeta_stop_mu, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    int missing = __cajeta_stop_expected - __cajeta_stop_parked;
    if (missing < 0) missing = 0;
    pthread_mutex_unlock(&__cajeta_stop_mu);
    return missing;
}

int __cajeta_stop_parked_count(void) {
    pthread_mutex_lock(&__cajeta_stop_mu);
    int n = __cajeta_stop_parked;
    pthread_mutex_unlock(&__cajeta_stop_mu);
    return n;
}

int __cajeta_stop_expected_count(void) {
    pthread_mutex_lock(&__cajeta_stop_mu);
    int n = __cajeta_stop_expected;
    pthread_mutex_unlock(&__cajeta_stop_mu);
    return n;
}

unsigned __cajeta_stop_generation_get(void) {
    pthread_mutex_lock(&__cajeta_stop_mu);
    unsigned g = __cajeta_stop_generation;
    pthread_mutex_unlock(&__cajeta_stop_mu);
    return g;
}

// Test-only: drop the coordinator back to its idle state.
void __cajeta_stop_reset(void) {
    pthread_mutex_lock(&__cajeta_stop_mu);
    __atomic_store_n(&__cajeta_stop_requested, 0, __ATOMIC_RELAXED);
    __cajeta_stop_generation = 0;
    __cajeta_stop_parked = 0;
    __cajeta_stop_expected = 0;
    pthread_mutex_unlock(&__cajeta_stop_mu);
}

// Per-fiber debug frame chain: under --debug-info, codegen emits a frame_enter
// at each prologue, a frame_leave on every return path, and a __cajeta_dbg_local
// per named local. CAVEAT: an exception unwind leaks its frame node.
#define CAJETA_DBG_MAX_LOCALS 64
struct cajeta_dbg_local {
    const char* name;
    const char* type;   // cajeta canonical type name (e.g. "int32", "demo.Foo")
    void* addr;          // the local slot: the value, or the heap pointer
    // Memory facets as plain bytes: alloc 0-3 = unknown/stack/heap/shared,
    // ownership 0-3 = unknown/owner/borrow/moved-out (the cajeta::dbg enums).
    uint8_t alloc;
    uint8_t ownership;
    // The owner's drop-chain entry (a cajeta_drop_entry*, whose `active` flag is
    // at the same offset in both shapes), or NULL. A void* to stay above its def.
    void* drop_entry;
};
struct cajeta_dbg_frame {
    const char* func;          // cajeta-mangled enclosing function name
    int32_t current_loc;       // loc_id of the last safepoint hit in this frame
    int nlocals;
    struct cajeta_dbg_local locals[CAJETA_DBG_MAX_LOCALS];
    struct cajeta_dbg_frame* prev;
    // The chain slot this frame was pushed onto: leave() must unlink from ITS
    // OWN chain, since a method can enter and leave under different fibers.
    struct cajeta_dbg_frame** owner;
};

// Selector for the dbg frame-chain head (fiber vs main TLS); defined below.
struct cajeta_dbg_frame** __cajeta_dbg_top_ptr(void);

// Push a debug frame for `func` onto this context's chain; returns the node.
void* __cajeta_dbg_frame_enter(const char* func) {
    struct cajeta_dbg_frame* f = malloc(sizeof(*f));
    if (!f) {
        fprintf(stderr, "cajeta: __cajeta_dbg_frame_enter malloc failed\n");
        abort();
    }
    f->func = func;
    f->current_loc = -1;
    f->nlocals = 0;
    struct cajeta_dbg_frame** top = __cajeta_dbg_top_ptr();
    f->prev = *top;
    f->owner = top;
    *top = f;
    return f;
}

// Unlink EXACTLY the node this call's matching enter pushed, from the chain it
// went onto. A node no longer on its owner chain is leaked, never re-linked.
void __cajeta_dbg_frame_leave(void* node) {
    struct cajeta_dbg_frame* f = (struct cajeta_dbg_frame*) node;
    if (!f) return;
    struct cajeta_dbg_frame** ow = f->owner;
    if (!ow) { free(f); return; }
    if (*ow == f) {
        *ow = f->prev;
    } else {
        struct cajeta_dbg_frame* p = *ow;
        int i = 0;
        while (p && p->prev != f && i++ < 65536) p = p->prev;
        if (p && p->prev == f) p->prev = f->prev;
        else return;   // not on its chain: someone unlinked it; do not free
    }
    free(f);
}

// --- diagnostic-exceptions: line-info shadow stack --------------------------
// A per-FIBER mirror of the active Cajeta frames, so a trace carries exact
// file:line with no DWARF. Never mallocs; an unwind resets the top.
#include "cajeta_prof_abi.h"


// One shadow stack: the program thread owns the __thread instance below.
typedef struct {
    CajetaShadowFrame frames[CAJETA_SHADOW_MAX];
    int32_t top;
    // How many INSTRUMENTATION probes are live on THIS fiber's stack. Per-fiber
    // for the same reason the frames are: a carrier hosts many fibers.
    int32_t instr_depth;
} CajetaShadowStack;

// Program/main-thread slot — for any thread that is not running a fiber.
static __thread CajetaShadowStack __cajeta_main_shadow;

// Selector for the live shadow stack; defined in cajeta_rt_concurrent_exec.c.
CajetaShadowStack* __cajeta_shadow_ptr(void);

// Push, update and pop the shadow frame for the running context. `top` counts
// past the cap so leave stays balanced with enter.
void __cajeta_line_enter(const void* desc) {
    CajetaShadowStack* s = __cajeta_shadow_ptr();
    int32_t t = s->top;
    if (t >= 0 && t < CAJETA_SHADOW_MAX) {
        s->frames[t].desc = (const CajetaFrameDesc*) desc;
        s->frames[t].line = 0;
    }
    s->top = t + 1;
}
void __cajeta_line_mark(int32_t line) {
    CajetaShadowStack* s = __cajeta_shadow_ptr();
    int32_t t = s->top;
    if (t > 0 && t <= CAJETA_SHADOW_MAX) s->frames[t - 1].line = line;
}
void __cajeta_line_leave(void) {
    CajetaShadowStack* s = __cajeta_shadow_ptr();
    if (s->top > 0) s->top--;
}
int32_t __cajeta_shadow_get_top(void) { return __cajeta_shadow_ptr()->top; }
void __cajeta_shadow_set_top(int32_t watermark) {
    if (watermark >= 0) __cajeta_shadow_ptr()->top = watermark;
}
// ── cajeta-profiler: live-thread registry ─────────────────────────────────
// Publishes each live PROGRAM THREAD's shadow stack so a sampler can read a
// stack it does not own. Fibers go through the fiber registry, which has them.
static pthread_mutex_t __cajeta_prof_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static CajetaShadowStack** __cajeta_prof_threads = NULL;
static int __cajeta_prof_thread_n = 0;
static int __cajeta_prof_thread_cap = 0;

// This thread's own shadow-stack handle — the PROGRAM-thread slot, never a fiber's.
void* __cajeta_prof_thread_self(void) {
    return (void*) &__cajeta_main_shadow;
}

void __cajeta_prof_thread_register(void) {
    CajetaShadowStack* self = &__cajeta_main_shadow;
    pthread_mutex_lock(&__cajeta_prof_thread_mutex);
    for (int i = 0; i < __cajeta_prof_thread_n; i++) {
        if (__cajeta_prof_threads[i] == self) {   // idempotent
            pthread_mutex_unlock(&__cajeta_prof_thread_mutex);
            return;
        }
    }
    if (__cajeta_prof_thread_n == __cajeta_prof_thread_cap) {
        int cap = __cajeta_prof_thread_cap ? __cajeta_prof_thread_cap * 2 : 16;
        CajetaShadowStack** grown = (CajetaShadowStack**) realloc(
            __cajeta_prof_threads, (size_t) cap * sizeof(CajetaShadowStack*));
        if (!grown) { pthread_mutex_unlock(&__cajeta_prof_thread_mutex); return; }
        __cajeta_prof_threads = grown;
        __cajeta_prof_thread_cap = cap;
    }
    __cajeta_prof_threads[__cajeta_prof_thread_n++] = self;
    pthread_mutex_unlock(&__cajeta_prof_thread_mutex);
}

void __cajeta_prof_thread_unregister(void) {
    CajetaShadowStack* self = &__cajeta_main_shadow;
    pthread_mutex_lock(&__cajeta_prof_thread_mutex);
    for (int i = 0; i < __cajeta_prof_thread_n; i++) {
        if (__cajeta_prof_threads[i] != self) continue;
        // Order-preserving removal, matching the fiber registry.
        for (int j = i + 1; j < __cajeta_prof_thread_n; j++)
            __cajeta_prof_threads[j - 1] = __cajeta_prof_threads[j];
        __cajeta_prof_thread_n--;
        break;
    }
    pthread_mutex_unlock(&__cajeta_prof_thread_mutex);
}

int __cajeta_prof_thread_count(void) {
    pthread_mutex_lock(&__cajeta_prof_thread_mutex);
    int n = __cajeta_prof_thread_n;
    pthread_mutex_unlock(&__cajeta_prof_thread_mutex);
    return n;
}

// Copy up to `max` live handles under one lock hold; returns the live count.
int __cajeta_prof_thread_snapshot(void** out, int max) {
    pthread_mutex_lock(&__cajeta_prof_thread_mutex);
    int n = __cajeta_prof_thread_n;
    if (out && max > 0) {
        int copy = n < max ? n : max;
        for (int i = 0; i < copy; i++) out[i] = (void*) __cajeta_prof_threads[i];
    }
    pthread_mutex_unlock(&__cajeta_prof_thread_mutex);
    return n;
}

// Snapshot another thread's shadow stack — the sampler's entry point. `handle`
// must be a CajetaShadowStack* (a FIBER handle goes through
// __cajeta_dbg_fiber_shadow_of first); `truncated` reports an over-cap depth.
int32_t __cajeta_prof_stack_snapshot(void* handle, CajetaShadowFrame* out,
                                     int32_t max, int32_t* truncated) {
    CajetaShadowStack* s = (CajetaShadowStack*) handle;
    if (!s) { if (truncated) *truncated = 0; return 0; }
    int32_t n = s->top;
    int32_t trunc = 0;
    if (n > CAJETA_SHADOW_MAX) { n = CAJETA_SHADOW_MAX; trunc = 1; }
    if (n < 0) n = 0;
    if (truncated) *truncated = trunc;
    if (!out || max <= 0) return n;   // count-only query
    int32_t w = 0;
    for (int32_t i = n - 1; i >= 0 && w < max; i--) out[w++] = s->frames[i];
    return w;
}

// ── cajeta-profiler: the sampler ──────────────────────────────────────────
// A dedicated thread walks the thread and fiber registries on an interval and
// copies each live stack into a fixed ring; on overflow it DROPS and counts.
int64_t __cajeta_currentTimeNanos(void);
long __cajeta_dbg_fiber_id_of(void* fiber);   // cajeta_rt_concurrent_exec.c
void* __cajeta_dbg_fiber_shadow_of(void* fiber);   // ditto

#define CAJETA_PROF_DEFAULT_HZ 1000
#define CAJETA_PROF_DEFAULT_RING 4096


static pthread_t        __cajeta_prof_thread;
static volatile int     __cajeta_prof_armed = 0;
static volatile int     __cajeta_prof_stop  = 0;
static int32_t          __cajeta_prof_interval = 0;
static CajetaProfSample* __cajeta_prof_ring = NULL;
static int32_t          __cajeta_prof_ring_cap = 0;
static volatile int64_t __cajeta_prof_head = 0;   // producer index, monotonic
static volatile int64_t __cajeta_prof_tail = 0;   // consumer index (Unit 5)
static volatile int64_t __cajeta_prof_samples = 0;
static volatile int64_t __cajeta_prof_drops = 0;
static volatile int64_t __cajeta_prof_frames = 0;
static const char*      __cajeta_prof_out = NULL;

// Codegen registers that line-info probes were emitted through a global ctor and
// NOT a weak extern, which would collide with codegen's strong definition.
static volatile int __cajeta_line_info_present_flag = 0;
void __cajeta_line_info_register(void) { __cajeta_line_info_present_flag = 1; }
int32_t __cajeta_line_info_is_present(void) { return __cajeta_line_info_present_flag; }

int32_t __cajeta_prof_interval_us(void)  { return __cajeta_prof_interval; }
int32_t __cajeta_prof_ring_capacity(void){ return __cajeta_prof_ring_cap; }
int32_t __cajeta_prof_is_armed(void)     { return __cajeta_prof_armed; }
int64_t __cajeta_prof_sample_count(void) { return __cajeta_prof_samples; }
int64_t __cajeta_prof_drop_count(void)   { return __cajeta_prof_drops; }
int64_t __cajeta_prof_frame_count(void)  { return __cajeta_prof_frames; }
const char* __cajeta_prof_out_path(void) {
    return __cajeta_prof_out ? __cajeta_prof_out : "cajeta.pftrace";
}

// Copy one stack into the ring. Producer-only, so head moves without a CAS.
static void __cajeta_prof_push(void* owner, int32_t owner_kind) {
    int32_t trunc = 0;
    CajetaShadowFrame tmp[CAJETA_PROF_MAX_FRAMES];
    // A THREAD handle IS its shadow stack; a FIBER handle is a cajeta_fiber*, so
    // resolve it. Getting this wrong is silent — the fiber lane just goes missing.
    void* stack = (owner_kind == CAJETA_PROF_OWNER_FIBER)
                      ? __cajeta_dbg_fiber_shadow_of(owner)
                      : owner;
    int32_t n = __cajeta_prof_stack_snapshot(stack, tmp,
                                             CAJETA_PROF_MAX_FRAMES, &trunc);
    __cajeta_prof_samples++;
    if (n <= 0) return;               // idle context: a tick, but no frames
    int64_t head = __cajeta_prof_head;
    if (head - __cajeta_prof_tail >= __cajeta_prof_ring_cap) {
        __cajeta_prof_drops++;        // full: drop, never block the sampler
        return;
    }
    CajetaProfSample* slot = &__cajeta_prof_ring[head % __cajeta_prof_ring_cap];
    slot->host_ns = __cajeta_currentTimeNanos();
    slot->owner = owner;
    slot->owner_kind = owner_kind;
    slot->owner_id = (owner_kind == CAJETA_PROF_OWNER_FIBER)
                         ? (int64_t) __cajeta_dbg_fiber_id_of(owner)
                         : 0;
    slot->n_frames = n;
    slot->truncated = trunc;
    for (int32_t i = 0; i < n; i++) slot->frames[i] = tmp[i];
    __cajeta_prof_frames += n;
    __atomic_store_n(&__cajeta_prof_head, head + 1, __ATOMIC_RELEASE);
}

static void* __cajeta_prof_loop(void* arg) {
    (void) arg;
    // The sampler does not register itself: its own stack is not the program's work.
    while (!__cajeta_prof_stop) {
        void* handles[256];
        int n = __cajeta_prof_thread_snapshot(handles, 256);
        if (n > 256) n = 256;
        for (int i = 0; i < n; i++)
            __cajeta_prof_push(handles[i], CAJETA_PROF_OWNER_THREAD);
        int fn = __cajeta_dbg_fiber_snapshot(handles, 256);
        if (fn > 256) fn = 256;
        for (int i = 0; i < fn; i++)
            __cajeta_prof_push(handles[i], CAJETA_PROF_OWNER_FIBER);
        struct timespec ts;
        ts.tv_sec  = __cajeta_prof_interval / 1000000;
        ts.tv_nsec = (long) (__cajeta_prof_interval % 1000000) * 1000L;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

// Returns 0 on success; negative on refusal. -1 already armed, -2 line-info is
// off (§2.5 — fail loudly rather than produce an empty trace), -3 out of
// memory, -4 the sampler thread could not start.
int32_t __cajeta_prof_arm(void) {
    if (__cajeta_prof_armed) return -1;
    if (!getenv("CAJETA_PROFILER")) return 0;   // unset: arm nothing (§9.1)
    if (!__cajeta_line_info_is_present()) {
        fprintf(stderr,
                "cajeta.profiler: refusing to arm — this binary was built "
                "with --debug-info=off, so there are no frames to sample. "
                "Rebuild with --debug-info=line (the default) to profile, or "
                "--debug-info=full to also get exact line numbers.\n");
        return -2;
    }
    const char* hz_s = getenv("CAJETA_PROFILER_HZ");
    int hz = hz_s ? atoi(hz_s) : CAJETA_PROF_DEFAULT_HZ;
    if (hz <= 0) hz = CAJETA_PROF_DEFAULT_HZ;
    __cajeta_prof_interval = 1000000 / hz;
    if (__cajeta_prof_interval <= 0) __cajeta_prof_interval = 1;

    const char* ring_s = getenv("CAJETA_PROFILER_RING");
    int cap = ring_s ? atoi(ring_s) : CAJETA_PROF_DEFAULT_RING;
    if (cap <= 0) cap = CAJETA_PROF_DEFAULT_RING;
    __cajeta_prof_ring_cap = cap;
    __cajeta_prof_ring = (CajetaProfSample*) calloc((size_t) cap,
                                                    sizeof(CajetaProfSample));
    if (!__cajeta_prof_ring) { __cajeta_prof_ring_cap = 0; return -3; }
    __cajeta_prof_out = getenv("CAJETA_PROFILER_OUT");
    // GPU capture arms from the same variable and HERE, before any backend
    // initializes: rocprofiler's configure hook only gets this one window.
    {
        const char* gring_s = getenv("CAJETA_PROFILER_GPU_RING");
        int gcap = gring_s ? atoi(gring_s) : 0;   // 0 = the capture default
        __cajeta_prof_gpu_capture_arm(gcap);
    }
    __cajeta_prof_head = __cajeta_prof_tail = 0;
    __cajeta_prof_stop = 0;
    // The arming thread is the program thread; register it here, not lazily.
    __cajeta_prof_thread_register();
    if (pthread_create(&__cajeta_prof_thread, NULL, __cajeta_prof_loop, NULL) != 0) {
        free(__cajeta_prof_ring);
        __cajeta_prof_ring = NULL;
        __cajeta_prof_ring_cap = 0;
        return -4;
    }
    __cajeta_prof_armed = 1;
    return 0;
}

void __cajeta_prof_disarm(void) {
    if (!__cajeta_prof_armed) return;
    __cajeta_prof_stop = 1;
    pthread_join(__cajeta_prof_thread, NULL);
    __cajeta_prof_armed = 0;
    // The ring is NOT freed here: the trace writer drains it on the way out.
}

// Snapshot the live shadow frames innermost-first into `out` (caller-sized to
// `max`), returning the number copied. `out[0]` is the throw-site frame.
int32_t __cajeta_shadow_snapshot(CajetaShadowFrame* out, int32_t max) {
    CajetaShadowStack* s = __cajeta_shadow_ptr();
    int32_t n = s->top;
    if (n > CAJETA_SHADOW_MAX) n = CAJETA_SHADOW_MAX;  // deepest-past-cap unstored
    int32_t w = 0;
    for (int32_t i = n - 1; i >= 0 && w < max; i--) out[w++] = s->frames[i];
    return w;
}

// Print the LIVE shadow stack to `fd` (1 stdout, 2 stderr) as
// `at Type.method(File.cajeta:NN)` — the Cajeta backtrace, callable from gdb.
// `used, retain`: nothing in generated code calls it, so DCE would drop it.
__attribute__((used, retain))
void __cajeta_print_stack(int32_t fd) {
    FILE* out = (fd == 1) ? stdout : stderr;
    CajetaShadowStack* s = __cajeta_shadow_ptr();
    int32_t n = s->top;
    if (n > CAJETA_SHADOW_MAX) n = CAJETA_SHADOW_MAX;
    if (n <= 0) {
        fprintf(out, "  <no cajeta frames: line-info off, or not in cajeta code>\n");
        fflush(out);
        return;
    }
    for (int32_t i = n - 1; i >= 0; i--) {
        const CajetaFrameDesc* d = s->frames[i].desc;
        const char* t = (d && d->typeName)   ? d->typeName   : "?";
        const char* m = (d && d->methodName) ? d->methodName : "?";
        const char* f = (d && d->fileName)   ? d->fileName   : "?";
        // Basename only, matching the captured-trace format.
        const char* base = f;
        for (const char* q = f; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
        fprintf(out, "  at %s.%s(%s:%d)\n", t, m, base, s->frames[i].line);
    }
    fflush(out);
}

// Depth of the live shadow stack, and one frame by index (0 = innermost).
__attribute__((used, retain))
int32_t __cajeta_stack_depth(void) {
    int32_t n = __cajeta_shadow_ptr()->top;
    return n > CAJETA_SHADOW_MAX ? CAJETA_SHADOW_MAX : (n < 0 ? 0 : n);
}
__attribute__((used, retain))
const char* __cajeta_stack_type(int32_t i) {
    int32_t n = __cajeta_stack_depth();
    if (i < 0 || i >= n) return "";
    const CajetaFrameDesc* d = __cajeta_shadow_ptr()->frames[n - 1 - i].desc;
    return (d && d->typeName) ? d->typeName : "?";
}
__attribute__((used, retain))
const char* __cajeta_stack_method(int32_t i) {
    int32_t n = __cajeta_stack_depth();
    if (i < 0 || i >= n) return "";
    const CajetaFrameDesc* d = __cajeta_shadow_ptr()->frames[n - 1 - i].desc;
    return (d && d->methodName) ? d->methodName : "?";
}
__attribute__((used, retain))
const char* __cajeta_stack_file(int32_t i) {
    int32_t n = __cajeta_stack_depth();
    if (i < 0 || i >= n) return "";
    const CajetaFrameDesc* d = __cajeta_shadow_ptr()->frames[n - 1 - i].desc;
    return (d && d->fileName) ? d->fileName : "?";
}
__attribute__((used, retain))
int32_t __cajeta_stack_line(int32_t i) {
    int32_t n = __cajeta_stack_depth();
    if (i < 0 || i >= n) return 0;
    return __cajeta_shadow_ptr()->frames[n - 1 - i].line;
}

// The embedded location table (external-debug §3): under --debug-info=full,
// codegen serializes loc_id -> {file, line, col, func} into the binary and a ctor
// registers it here. A `line` or `off` build registers nothing, and answers benignly.
typedef struct {
    const char* file;
    int32_t     line;
    int32_t     col;
    const char* func;
} CajetaDbgLocEntry;

static const CajetaDbgLocEntry* __cajeta_dbg_loc_entries = 0;
static int32_t __cajeta_dbg_loc_n = 0;

// Called from the ctor codegen emits under --debug-info=full; null/empty CLEARS.
__attribute__((used, retain))
void __cajeta_dbg_register_loc_table(const CajetaDbgLocEntry* entries,
                                     int32_t count) {
    if (!entries || count <= 0) {
        __cajeta_dbg_loc_entries = 0;
        __cajeta_dbg_loc_n = 0;
        return;
    }
    __cajeta_dbg_loc_entries = entries;
    __cajeta_dbg_loc_n = count;
}

__attribute__((used, retain))
int32_t __cajeta_dbg_loc_count(void) {
    return __cajeta_dbg_loc_entries ? __cajeta_dbg_loc_n : 0;
}

__attribute__((used, retain))
const char* __cajeta_dbg_loc_file(int32_t id) {
    if (!__cajeta_dbg_loc_entries || id < 0 || id >= __cajeta_dbg_loc_n) return "";
    const char* f = __cajeta_dbg_loc_entries[id].file;
    return f ? f : "";
}

__attribute__((used, retain))
int32_t __cajeta_dbg_loc_line(int32_t id) {
    if (!__cajeta_dbg_loc_entries || id < 0 || id >= __cajeta_dbg_loc_n) return 0;
    return __cajeta_dbg_loc_entries[id].line;
}

__attribute__((used, retain))
int32_t __cajeta_dbg_loc_col(int32_t id) {
    if (!__cajeta_dbg_loc_entries || id < 0 || id >= __cajeta_dbg_loc_n) return 0;
    return __cajeta_dbg_loc_entries[id].col;
}

__attribute__((used, retain))
const char* __cajeta_dbg_loc_func(int32_t id) {
    if (!__cajeta_dbg_loc_entries || id < 0 || id >= __cajeta_dbg_loc_n) return "";
    const char* f = __cajeta_dbg_loc_entries[id].func;
    return f ? f : "";
}

// The ids a line breakpoint on (file, line) must arm; `file` matches by suffix on
// a path boundary. Writes up to `max` into `out` and returns the count found.
__attribute__((used, retain))
int32_t __cajeta_dbg_ids_for_line(const char* file, int32_t line,
                                  int32_t* out, int32_t max) {
    if (!__cajeta_dbg_loc_entries || !file) return 0;
    size_t flen = strlen(file);
    int32_t found = 0;
    for (int32_t i = 0; i < __cajeta_dbg_loc_n; ++i) {
        const CajetaDbgLocEntry* e = &__cajeta_dbg_loc_entries[i];
        if (e->line != line || !e->file) continue;
        size_t elen = strlen(e->file);
        if (elen < flen) continue;
        if (strcmp(e->file + (elen - flen), file) != 0) continue;
        // The suffix must start at a path boundary: "Guid.cajeta" is not "MyGuid.cajeta".
        if (elen > flen && e->file[elen - flen - 1] != '/') continue;
        if (out && found < max) out[found] = i;
        found++;
    }
    return found;
}

// Record a named local in the innermost debug frame; drops silently past the cap.
void __cajeta_dbg_local(const char* name, const char* type, void* addr,
                        uint8_t alloc, uint8_t ownership, void* drop_entry) {
    struct cajeta_dbg_frame** top = __cajeta_dbg_top_ptr();
    struct cajeta_dbg_frame* f = *top;
    if (!f || f->nlocals >= CAJETA_DBG_MAX_LOCALS) return;
    f->locals[f->nlocals].name = name;
    f->locals[f->nlocals].type = type;
    f->locals[f->nlocals].addr = addr;
    f->locals[f->nlocals].alloc = alloc;
    f->locals[f->nlocals].ownership = ownership;
    f->locals[f->nlocals].drop_entry = drop_entry;
    f->nlocals++;
}

// Stateless host-side accessors: the chain is built by the JIT'd copy and read
// through the NATIVE one, so each is pointer arithmetic on a passed-in void*.

// The chain head, for a debugger with no frame pointer to start from.
__attribute__((used, retain))
void* __cajeta_dbg_frame_top(void) {
    struct cajeta_dbg_frame** top = __cajeta_dbg_top_ptr();
    return top ? *top : NULL;
}

__attribute__((used, retain))
int __cajeta_dbg_frame_depth(void* top) {
    int n = 0;
    for (struct cajeta_dbg_frame* f = top; f; f = f->prev) n++;
    return n;
}
__attribute__((used, retain))
void* __cajeta_dbg_frame_prev(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->prev : NULL;
}
__attribute__((used, retain))
const char* __cajeta_dbg_frame_func(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->func : NULL;
}
__attribute__((used, retain))
int32_t __cajeta_dbg_frame_loc(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->current_loc : -1;
}
__attribute__((used, retain))
int __cajeta_dbg_frame_nlocals(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->nlocals : 0;
}
__attribute__((used, retain))
const char* __cajeta_dbg_local_name(void* frame, int i) {
    if (!frame) return NULL;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return NULL;
    return f->locals[i].name;
}
__attribute__((used, retain))
const char* __cajeta_dbg_local_type(void* frame, int i) {
    if (!frame) return NULL;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return NULL;
    return f->locals[i].type;
}
__attribute__((used, retain))
void* __cajeta_dbg_local_addr(void* frame, int i) {
    if (!frame) return NULL;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return NULL;
    return f->locals[i].addr;
}
// The two memory facets; out of range reads back 0 (== Unknown).
__attribute__((used, retain))
uint8_t __cajeta_dbg_local_alloc(void* frame, int i) {
    if (!frame) return 0;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return 0;
    return f->locals[i].alloc;
}
__attribute__((used, retain))
uint8_t __cajeta_dbg_local_ownership(void* frame, int i) {
    if (!frame) return 0;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return 0;
    return f->locals[i].ownership;
}

// The handler also receives the frame-chain head, which the host TLS cannot reach.
typedef void (*cajeta_dbg_handler_fn)(int32_t loc_id, int fiber_id,
                                      void* frame_top);
static cajeta_dbg_handler_fn __cajeta_dbg_handler = NULL;
int __cajeta_dbg_current_fiber_id(void);

void __cajeta_dbg_set_safepoint_handler(cajeta_dbg_handler_fn fn) {
    __cajeta_dbg_handler = fn;
}

// The notebook interrupt (jupyter-kernel spec 5.1). A safepoint is the one place
// a running cell is known to be between statements. Set from another thread, so
// atomic; NOT static, or the JIT's partitioning can duplicate it per partition.
volatile int __cajeta_session_interrupt_flag = 0;

// Defined in cajeta_rt_session.c: unwinds to the session guard, and may not return.
void __cajeta_session_interrupt_unwind(void);

void __cajeta_session_request_interrupt(void) {
    __atomic_store_n(&__cajeta_session_interrupt_flag, 1, __ATOMIC_RELAXED);
}

// Cleared before every cell, so a stale flag cannot kill the NEXT one.
void __cajeta_session_clear_interrupt(void) {
    __atomic_store_n(&__cajeta_session_interrupt_flag, 0, __ATOMIC_RELAXED);
}

int __cajeta_session_interrupt_pending(void) {
    return __atomic_load_n(&__cajeta_session_interrupt_flag, __ATOMIC_RELAXED);
}

// TEST SEAM: trip the interrupt at the Nth safepoint from now, deterministically
// on the running thread, so the repro needs no loop. Negative disarms; 0 is off.
int __cajeta_session_interrupt_countdown = -1;

void __cajeta_session_interrupt_arm_after(int32_t n) {
    __atomic_store_n(&__cajeta_session_interrupt_countdown, n, __ATOMIC_RELAXED);
}

// The per-statement safepoint: take any pending interrupt, record `loc_id` in the
// innermost frame, run the installed handler, then park if a stop is in flight.
void __cajeta_dbg_safepoint(int32_t loc_id) {
    __cajeta_dbg_safepoint_total++;
    // Take the request rather than test it: whoever unwinds owns it.
    {
        int take = __atomic_exchange_n(&__cajeta_session_interrupt_flag, 0,
                                       __ATOMIC_RELAXED);
        // The test seam's countdown, checked only while armed.
        int n = __atomic_load_n(&__cajeta_session_interrupt_countdown,
                                __ATOMIC_RELAXED);
        if (n > 0) {
            n -= 1;
            __atomic_store_n(&__cajeta_session_interrupt_countdown, n,
                             __ATOMIC_RELAXED);
            if (n == 0) take = 1;
        }
        if (take) {
            __atomic_store_n(&__cajeta_session_interrupt_countdown, -1,
                             __ATOMIC_RELAXED);
            __cajeta_session_interrupt_unwind();
        }
    }
    // Record the line so a multi-frame stackTrace shows each frame's statement.
    struct cajeta_dbg_frame* top = *__cajeta_dbg_top_ptr();
    if (top) top->current_loc = loc_id;
    cajeta_dbg_handler_fn h = __cajeta_dbg_handler;
    if (h) h(loc_id, __cajeta_dbg_current_fiber_id(), top);
    // Cross-carrier convergence: every carrier but the armed one parks here as a
    // secondary, so no fiber advances past a safepoint while the world is stopped.
    if (__cajeta_stop_is_requested()) __cajeta_stop_park();
}

// Settable exception handler: __cajeta_throw calls it at the throw chokepoint,
// BEFORE the stack unwinds, so the throwing frame chain is still intact. NULL by
// default. Receives the Throwable*, the fiber id, and the frame-chain head.
typedef void (*cajeta_dbg_exception_fn)(void* throwable, int fiber_id,
                                        void* frame_top);
// NOT static: under the JIT's partitioned materialization an `internal` global
// read from a distant function can be duplicated, and the handler never fires.
cajeta_dbg_exception_fn __cajeta_dbg_exception_handler = NULL;

void __cajeta_dbg_set_exception_handler(cajeta_dbg_exception_fn fn) {
    __cajeta_dbg_exception_handler = fn;
}

long __cajeta_dbg_safepoint_count(void) {
    return __cajeta_dbg_safepoint_total;
}

// The PROGRAM thread's identity. A carrier outside fiber context must not report
// fiber id 0: that impersonates the program thread and steals its pending steps.
static pthread_t __cajeta_dbg_program_thread;
static int __cajeta_dbg_program_thread_set = 0;

int __cajeta_dbg_on_program_thread(void) {
    return __cajeta_dbg_program_thread_set
        && pthread_equal(pthread_self(), __cajeta_dbg_program_thread);
}

void __cajeta_dbg_reset_safepoint_count(void) {
    __cajeta_dbg_safepoint_total = 0;
    // Fallback capture for the same-thread runner; a debug session re-marks below.
    if (!__cajeta_dbg_program_thread_set) {
        __cajeta_dbg_program_thread = pthread_self();
        __cajeta_dbg_program_thread_set = 1;
    }
}

// Called as the program thread's FIRST act, and re-marked once per session.
__attribute__((used, retain))
void __cajeta_dbg_mark_program_thread(void) {
    __cajeta_dbg_program_thread = pthread_self();
    __cajeta_dbg_program_thread_set = 1;
}

// Is `node` on the chain headed at `top`? The pending-step gate uses it so a
// candidate on a FOREIGN chain cannot satisfy a step. Bounded against cycles.
__attribute__((used, retain))
int __cajeta_dbg_frame_contains(void* top, void* node) {
    if (!node) return 0;
    const struct cajeta_dbg_frame* f = (const struct cajeta_dbg_frame*) top;
    for (int i = 0; f && i < 65536; ++i) {
        if ((const void*) f == node) return 1;
        f = f->prev;
    }
    return 0;
}

// Live-allocation set (FieldOwnership.md § Solution B): every heap block handed
// out is tracked, and drop dispatchers remove-and-claim so only the first frees.
#define CAJETA_LIVE_SET_INITIAL_CAPACITY (1 << 18)
                                             // 256K slots, 2 MB: a large owned-key map never rehashes.
#define CAJETA_LIVE_SET_TOMBSTONE ((void*) 1)  // page 0 unmapped; safe sentinel

static void** __cajeta_live_set = NULL;
static size_t __cajeta_live_set_cap = 0;
static int __cajeta_live_set_count = 0;
// Tombstoned slots. Counted because they occupy probe sequence like live ones.
static size_t __cajeta_live_set_tombstones = 0;
// Set once a growth allocation fails; the table then warns once and stops.
static int __cajeta_live_set_frozen = 0;

// Insert with no bookkeeping or growth check; takes the table for the rehash.
static int __cajeta_live_set_insert_into(void** table, size_t cap, void* p) {
    size_t mask = cap - 1;
    size_t bucket = ((uintptr_t) p >> 3) & mask;
    for (size_t i = 0; i < cap; i++) {
        size_t idx = (bucket + i) & mask;
        void* cur = table[idx];
        if (cur == NULL || cur == CAJETA_LIVE_SET_TOMBSTONE) {
            table[idx] = p;
            return 1;
        }
        if (cur == p) return 0;  // already present (alloc gave a fresh address)
    }
    return 0;
}

// Grow to `want` slots, or rehash in place when tombstones rather than live
// entries filled the table. Returns 1 on success; on failure the set freezes.
static int __cajeta_live_set_rehash(size_t want) {
    void** old = __cajeta_live_set;
    size_t oldCap = __cajeta_live_set_cap;
    void** fresh = (void**) calloc(want, sizeof(void*));
    if (fresh == NULL) {
        __cajeta_live_set_frozen = 1;
        return 0;
    }
    if (old != NULL) {
        for (size_t i = 0; i < oldCap; i++) {
            void* cur = old[i];
            if (cur != NULL && cur != CAJETA_LIVE_SET_TOMBSTONE) {
                __cajeta_live_set_insert_into(fresh, want, cur);
            }
        }
        free(old);
    }
    __cajeta_live_set = fresh;
    __cajeta_live_set_cap = want;
    __cajeta_live_set_tombstones = 0;   // rehash drops every tombstone
    return 1;
}
static pthread_mutex_t __cajeta_live_set_mu = PTHREAD_MUTEX_INITIALIZER;

// Current live-object population. Test-only introspection (Cajeta.liveCount()).
int64_t __cajeta_live_set_population(void) {
    return (int64_t) __cajeta_live_set_count;
}

// Cumulative bytes ever requested across cajeta's allocation entry points, the
// metric Cajeta.allocatedBytes() samples as a delta. A relaxed atomic add.
static int64_t __cajeta_total_allocated = 0;

static inline void __cajeta_note_alloc(uint64_t bytes) {
    __atomic_fetch_add(&__cajeta_total_allocated, (int64_t) bytes, __ATOMIC_RELAXED);
}

int64_t __cajeta_total_allocated_bytes(void) {
    return __atomic_load_n(&__cajeta_total_allocated, __ATOMIC_RELAXED);
}

static void __cajeta_live_set_add_locked(void* p) {
    if (!p || p == CAJETA_LIVE_SET_TOMBSTONE) return;
    if (__cajeta_live_set == NULL) {
        if (!__cajeta_live_set_rehash(CAJETA_LIVE_SET_INITIAL_CAPACITY)) return;
    }
    // Grow at 75% OCCUPANCY — live entries plus tombstones both consume probe
    // sequence. A tombstone-filled table rehashes at the same size, not double.
    size_t used = (size_t) __cajeta_live_set_count + __cajeta_live_set_tombstones;
    if (!__cajeta_live_set_frozen
            && used >= (__cajeta_live_set_cap * 3) / 4) {
        size_t want = ((size_t) __cajeta_live_set_count
                           >= __cajeta_live_set_cap / 2)
            ? __cajeta_live_set_cap * 2
            : __cajeta_live_set_cap;
        __cajeta_live_set_rehash(want);
    }
    if (__cajeta_live_set_frozen
            && (size_t) __cajeta_live_set_count
                   >= (__cajeta_live_set_cap * 3) / 4) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                "cajeta: live-allocation set could not grow past %zu slots "
                "(out of memory). Subsequent allocations won't be tracked, so "
                "they will never be reclaimed — expect a leak proportional to "
                "how far past this point the program runs.\n",
                __cajeta_live_set_cap);
            warned = 1;
        }
        return;
    }
    if (__cajeta_live_set_insert_into(__cajeta_live_set,
                                      __cajeta_live_set_cap, p)) {
        __cajeta_live_set_count++;
    }
}

static int __cajeta_live_set_remove_locked(void* p) {
    if (!p || p == CAJETA_LIVE_SET_TOMBSTONE) return 0;
    if (__cajeta_live_set == NULL) return 0;
    size_t cap = __cajeta_live_set_cap;
    size_t mask = cap - 1;
    size_t bucket = ((uintptr_t) p >> 3) & mask;
    for (size_t i = 0; i < cap; i++) {
        size_t idx = (bucket + i) & mask;
        void* cur = __cajeta_live_set[idx];
        if (cur == NULL) return 0;  // empty slot — search ends
        if (cur == p) {
            __cajeta_live_set[idx] = CAJETA_LIVE_SET_TOMBSTONE;
            __cajeta_live_set_count--;
            __cajeta_live_set_tombstones++;
            return 1;
        }
    }
    return 0;
}

// Single-threaded fast path: the mutex only guards against a SECOND thread.
// One-way 0->1, flipped on the main thread BEFORE any worker thread is created.
static volatile int __cajeta_live_set_mt = 0;

void __cajeta_live_set_go_multithreaded(void) {
    __atomic_store_n(&__cajeta_live_set_mt, 1, __ATOMIC_RELEASE);
}

// Forces the locked live-set path on WITHOUT a second thread, so the cost of the
// flip can be measured alone. Absent $CAJETA_LIVE_SET_MT this is a no-op.
__attribute__((constructor))
static void __cajeta_live_set_mt_from_env(void) {
    const char* e = getenv("CAJETA_LIVE_SET_MT");
    if (e != NULL && *e != '\0' && *e != '0') {
        __atomic_store_n(&__cajeta_live_set_mt, 1, __ATOMIC_RELEASE);
    }
}

void __cajeta_live_set_add(void* p) {
    if (__atomic_load_n(&__cajeta_live_set_mt, __ATOMIC_ACQUIRE) == 0) {
        __cajeta_live_set_add_locked(p);
        return;
    }
    pthread_mutex_lock(&__cajeta_live_set_mu);
    __cajeta_live_set_add_locked(p);
    pthread_mutex_unlock(&__cajeta_live_set_mu);
}

// Remove `p`, returning 1 iff present — the atomic claim only one caller wins.
int __cajeta_live_set_claim(void* p) {
    if (__atomic_load_n(&__cajeta_live_set_mt, __ATOMIC_ACQUIRE) == 0) {
        return __cajeta_live_set_remove_locked(p);
    }
    pthread_mutex_lock(&__cajeta_live_set_mu);
    int r = __cajeta_live_set_remove_locked(p);
    pthread_mutex_unlock(&__cajeta_live_set_mu);
    return r;
}

// Per-thread paired RETURN flag (title-tracking §4.2/§4.4): a class-pointer
// return stores title(1)/borrow(0) just before `ret`, read right after the call.
static __thread int64_t __cajeta_return_flag_tls = 0;
int64_t __cajeta_return_flag_get(void) { return __cajeta_return_flag_tls; }
void __cajeta_return_flag_set(int64_t f) { __cajeta_return_flag_tls = f; }

// Allocate and zero-fill total_count elements of elem_size (primitive arrays).
void* __cajeta_new_array(uint64_t elem_size, uint64_t total_count) {
    if (total_count == 0) {
        return NULL;
    }
    void* buf = calloc((size_t) total_count, (size_t) elem_size);
    if (buf == NULL) {
        fprintf(stderr, "cajeta: __cajeta_new_array failed (count=%llu, size=%llu)\n",
                (unsigned long long) total_count, (unsigned long long) elem_size);
        abort();
    }
    __cajeta_note_alloc(total_count * elem_size);
    __cajeta_live_set_add(buf);
    return buf;
}

// Same as above, then run `ctor` on each element. Used for class-element arrays.
void* __cajeta_new_class_array(uint64_t elem_size, uint64_t total_count, cajeta_ctor_fn ctor) {
    void* buf = __cajeta_new_array(elem_size, total_count);
    if (buf == NULL || ctor == NULL) {
        return buf;
    }
    char* p = (char*) buf;
    for (uint64_t i = 0; i < total_count; i++) {
        ctor(p);
        p += elem_size;
    }
    return buf;
}

// Allocate a Java-style array header — { i64 size, [count x elem] } — as one
// zeroed block with `count` at offset 0; `header_size` is the data region's offset.
void* __cajeta_new_array_header(uint64_t header_size, uint64_t elem_size, uint64_t count) {
    // Overflow guard: header + count*elem wraps in uint64 (`new int[-1]` arrives
    // as 0xFFFF...) and calloc(1, total) performs no nmemb*size check.
    if (elem_size != 0 && count > (UINT64_MAX - header_size) / elem_size) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header overflow (header=%llu elem=%llu count=%llu)\n",
                (unsigned long long) header_size,
                (unsigned long long) elem_size,
                (unsigned long long) count);
        abort();
    }
    uint64_t total = header_size + count * elem_size;
    if (total == 0) {
        return NULL;
    }
    void* hdr = calloc(1, (size_t) total);
    if (hdr == NULL) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header failed (header=%llu elem=%llu count=%llu)\n",
                (unsigned long long) header_size,
                (unsigned long long) elem_size,
                (unsigned long long) count);
        abort();
    }
    *((int64_t*) hdr) = (int64_t) count;
    __cajeta_note_alloc(total);
    __cajeta_live_set_add(hdr);
    return hdr;
}

// A droppable-element array carries a per-slot ownership bitmap in a TAIL:
// { i64 count | data[count*elem] | bits[ceil(count/8)] }, one zeroed block.
void* __cajeta_new_array_header_bits(uint64_t header_size, uint64_t elem_size, uint64_t count) {
    // Bits math is safe well below this; the elem guard below does the rest.
    if (count > (UINT64_MAX / 8) - 8) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header_bits overflow (count=%llu)\n",
                (unsigned long long) count);
        abort();
    }
    uint64_t bits = (count + 7) / 8;
    if (elem_size != 0 && count > (UINT64_MAX - header_size - bits) / elem_size) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header_bits overflow (header=%llu elem=%llu count=%llu)\n",
                (unsigned long long) header_size,
                (unsigned long long) elem_size,
                (unsigned long long) count);
        abort();
    }
    uint64_t total = header_size + count * elem_size + bits;
    if (total == 0) {
        return NULL;
    }
    void* hdr = calloc(1, (size_t) total);
    if (hdr == NULL) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header_bits failed (header=%llu elem=%llu count=%llu)\n",
                (unsigned long long) header_size,
                (unsigned long long) elem_size,
                (unsigned long long) count);
        abort();
    }
    *((int64_t*) hdr) = (int64_t) count;
    __cajeta_note_alloc(total);
    __cajeta_live_set_add(hdr);
    return hdr;
}

// __cajeta_new_array_header with the data left UNINITIALIZED (malloc), for a
// buffer the caller overwrites. Still live-set tracked, so drop is identical.
void* __cajeta_new_array_header_uninit(uint64_t header_size, uint64_t elem_size, uint64_t count) {
    if (elem_size != 0 && count > (UINT64_MAX - header_size) / elem_size) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header_uninit overflow (header=%llu elem=%llu count=%llu)\n",
                (unsigned long long) header_size,
                (unsigned long long) elem_size,
                (unsigned long long) count);
        abort();
    }
    uint64_t total = header_size + count * elem_size;
    if (total == 0) {
        return NULL;
    }
    void* hdr = malloc((size_t) total);
    if (hdr == NULL) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header_uninit failed (header=%llu elem=%llu count=%llu)\n",
                (unsigned long long) header_size,
                (unsigned long long) elem_size,
                (unsigned long long) count);
        abort();
    }
    *((int64_t*) hdr) = (int64_t) count;
    __cajeta_note_alloc(total);
    __cajeta_live_set_add(hdr);
    return hdr;
}

void* __cajeta_alloc(uint64_t size);  // defined below; used by __cajeta_args_make

// The ambient argv store (cajeta_rt_lang.c), which __cajeta_args_make READS.
int64_t     __cajeta_args_count(void);
const char* __cajeta_args_get(int64_t index);

// Materialize a cajeta `String[]` for `main(String[] args)` FROM THE AMBIENT
// STORE, so the JIT and exe hosts cannot disagree on argv slicing. The String
// size, offsets and vtable come from the emit shim; elements are owned Strings.
void* __cajeta_args_make(void* string_vtable, int64_t str_size,
                         int64_t off_lentag, int64_t off_aux,
                         int64_t off_base, int64_t off_cplen) {
    int64_t argc = __cajeta_args_count();
    if (argc < 0) argc = 0;
    // A String[] slot is str_size wide but holds a String* in its first 8 bytes.
    // Offsets are (lenTag, aux, base, cachedCpLength); aux and base are contiguous.
    void* arr = __cajeta_new_array_header(8, (uint64_t) str_size, (uint64_t) argc);
    char* base = (char*) arr + 8;
    for (int64_t i = 0; i < argc; i++) {
        const char* s = __cajeta_args_get(i);
        if (!s) s = "";
        int64_t len = (int64_t) strlen(s);
        void* str = __cajeta_alloc((uint64_t) str_size);
        *(void**)   ((char*) str)             = string_vtable;
        *(int32_t*) ((char*) str + off_cplen) = -1;
        if (len <= 12) {
            *(int32_t*) ((char*) str + off_lentag) = (int32_t) len;
            memset((char*) str + off_aux, 0, 12);
            memcpy((char*) str + off_aux, s, (size_t) len);
        } else {
            // Owned root: CajetaArray { i64 count=len, text, NUL }.
            void* bytes = __cajeta_new_array_header(8, 1, (uint64_t) (len + 1));
            *((int64_t*) bytes) = len;
            memcpy((char*) bytes + 8, s, (size_t) len + 1);
            *(int32_t*) ((char*) str + off_lentag) = (int32_t) len;
            *(int32_t*) ((char*) str + off_aux)    = 0;
            *(void**)   ((char*) str + off_base)   = bytes;
        }
        *(void**) (base + (size_t) i * (size_t) str_size) = str;
    }
    return arr;
}

// Idempotent (FieldOwnership.md § Solution B): only the claim winner frees.
int __cajeta_shared_owner_drop(void* base);   // cajeta_rt_shared.c (same TU)

void __cajeta_free_array(void* ptr) {
    if (!ptr) return;
    // Shared-state seam (slice-spec §3.6): only the last stake actually frees.
    if (!__cajeta_shared_owner_drop(ptr)) return;
    __cajeta_poison_buffer(ptr);
    free(ptr);
}

// Drop the live elements of an OWNING container's backing array before the buffer
// itself. `count` is the container's live count, NOT the header word (capacity);
// `header` and `stride` mirror the allocation, the reference at each slot's base.
void __cajeta_drop_array_elements(void* arr, int64_t count, int64_t header,
                                  int64_t stride, void (*drop_fn)(void*)) {
    if (arr == NULL || drop_fn == NULL || stride <= 0) return;
    char* data = (char*) arr + header;
    for (int64_t i = 0; i < count; i++) {
        void* elem = *(void**) (data + i * stride);
        if (elem != NULL) drop_fn(elem);
    }
}

// Drop helper for OWNING views: a view points into a byte[]'s data region, so
// scope exit must free the array HEADER. Its own symbol keeps the drop-fn shape.
void __cajeta_view_drop_owned(void* data_ptr) {
    if (data_ptr == NULL) return;
    void* header = (void*) ((char*) data_ptr - 8);
    if (!__cajeta_shared_owner_drop(header)) return;
    __cajeta_poison_buffer(header);
    free(header);
}

// Materialize a heap T[] from a view's variable-size T[] field: the view's bytes
// are packed elements, so allocate a header + data block and copy them in.
void* __cajeta_array_view_to_owned(const void* data, int64_t count, int64_t elem_size) {
    if (count < 0) count = 0;
    if (elem_size <= 0) elem_size = 1;
    uint64_t header_size = 8;
    // Same overflow guard as __cajeta_new_array_header: the product can still wrap.
    if ((uint64_t) count > (UINT64_MAX - header_size) / (uint64_t) elem_size) {
        fprintf(stderr, "cajeta: __cajeta_array_view_to_owned overflow (count=%lld elem=%lld)\n",
                (long long) count, (long long) elem_size);
        abort();
    }
    uint64_t total = header_size + (uint64_t) count * (uint64_t) elem_size;
    void* hdr = calloc(1, (size_t) total);
    if (hdr == NULL) {
        fprintf(stderr, "cajeta: __cajeta_array_view_to_owned failed (count=%lld elem=%lld)\n",
                (long long) count, (long long) elem_size);
        abort();
    }
    *((int64_t*) hdr) = (int64_t) count;
    if (data != NULL && count > 0) {
        memcpy((char*) hdr + header_size, data, (size_t) count * (size_t) elem_size);
    }
    __cajeta_live_set_add(hdr);
    return hdr;
}

// Zero-filled allocation for compiler-emitted heap blocks with no array shape.
void* __cajeta_alloc(uint64_t size) {
    if (size == 0) return NULL;
    void* p = calloc(1, (size_t) size);
    if (p == NULL) {
        fprintf(stderr, "cajeta: __cajeta_alloc failed (size=%llu)\n",
                (unsigned long long) size);
        abort();
    }
    __cajeta_note_alloc(size);
    __cajeta_live_set_add(p);
    return p;
}

// Uninitialized counterpart of __cajeta_alloc; still live-set tracked.
void* __cajeta_alloc_uninit(uint64_t size) {
    if (size == 0) return NULL;
    void* p = malloc((size_t) size);
    if (p == NULL) {
        fprintf(stderr, "cajeta: __cajeta_alloc_uninit failed (size=%llu)\n",
                (unsigned long long) size);
        abort();
    }
    __cajeta_note_alloc(size);
    __cajeta_live_set_add(p);
    return p;
}

// Mirror of __cajeta_free_array for non-array blocks, separate so the drop-fn
// types match. Unconditional: virtual_drop already claimed the instance.
void __cajeta_free(void* ptr) {
    if (!ptr) return;
    __cajeta_live_set_claim(ptr);  // remove if present; ignore result
    __cajeta_poison_buffer(ptr);
    free(ptr);
}

// ---- Frame bump arena (docs/specs/frame-arena-spec.md) ---------------------
// Bump allocator for non-escaping owned locals: allocate = bump, reclaim = O(1)
// reset to a mark. Not live-set tracked, never freed one by one; marks nest LIFO.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>          // VirtualAlloc/VirtualFree — Windows has no mmap
#else
#  include <sys/mman.h>
#endif

#define CAJETA_ARENA_RESERVE        ((size_t) 4 << 30)   // 4 GiB virtual / thread
#define CAJETA_ARENA_TRIM_THRESHOLD ((size_t) 4 << 20)   // trim retained > 4 MiB over mark

typedef struct {
    unsigned char* base;       // mmap base; NULL until first use
    size_t bump;               // current offset (bytes in use)
    size_t retained;           // high-water offset with pages physically committed
    size_t count;              // live arena-array allocations, drop-counted at reset
    size_t committed;          // Windows: bytes VirtualAlloc(MEM_COMMIT)'d so far;
                               // POSIX: set to RESERVE (the kernel commits on fault)
} cajeta_arena;

// The NON-FIBER arena: main thread plus native helpers. A carrier-hosted fiber
// owns one inside its `struct cajeta_fiber` — the LIFO discipline is per stack.
static __thread cajeta_arena __cajeta_arena = { NULL, 0, 0, 0, 0 };

// The current context's arena: the running fiber's own, else the thread's.
static cajeta_arena* __cajeta_arena_ptr(void);

// --- arena-mapping pool ------------------------------------------------------
// A dead fiber's mapping lands here and the next arena init reuses it.
struct cajeta_arena_pooled {
    unsigned char* base;
    size_t retained;
    size_t committed;
    struct cajeta_arena_pooled* next;
};
static struct cajeta_arena_pooled* __cajeta_arena_pool = NULL;
static pthread_mutex_t __cajeta_arena_pool_mu = PTHREAD_MUTEX_INITIALIZER;

// Defined in cajeta_rt_concurrent_sync.c; the arena reset bulk-counts drops here.
void __cajeta_drop_count_add(int64_t n);

static void __cajeta_arena_init(cajeta_arena* a) {
    // Reuse a pooled mapping from a completed fiber when one is available.
    pthread_mutex_lock(&__cajeta_arena_pool_mu);
    struct cajeta_arena_pooled* pooled = __cajeta_arena_pool;
    if (pooled) __cajeta_arena_pool = pooled->next;
    pthread_mutex_unlock(&__cajeta_arena_pool_mu);
    if (pooled) {
        a->base = pooled->base;
        a->retained = pooled->retained;
        a->committed = pooled->committed;
        a->bump = 0;
        free(pooled);
        return;
    }
#if defined(_WIN32)
    // No MAP_NORESERVE on Windows: reserve now, commit as the bump advances.
    void* p = VirtualAlloc(NULL, CAJETA_ARENA_RESERVE, MEM_RESERVE, PAGE_READWRITE);
    if (!p) {
        fprintf(stderr, "cajeta: arena VirtualAlloc(MEM_RESERVE) failed\n");
        abort();
    }
    a->committed = 0;
#else
    void* p = mmap(NULL, CAJETA_ARENA_RESERVE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "cajeta: arena mmap failed\n");
        abort();
    }
    a->committed = CAJETA_ARENA_RESERVE;   // kernel commits on first touch
#endif
    a->base = (unsigned char*) p;
    a->bump = 0;
    a->retained = 0;
}

// Return a dead fiber's mapping to the pool (no-op if it never allocated), handing
// retained pages back to the OS first so an outlier cannot pin RSS in the pool.
static void __cajeta_arena_release_mapping(cajeta_arena* a) {
    if (!a->base) return;
    if (a->retained > CAJETA_ARENA_TRIM_THRESHOLD) {
#if defined(_WIN32)
        VirtualFree(a->base, a->retained, MEM_DECOMMIT);
        a->committed = 0;
#else
        madvise(a->base, a->retained, MADV_DONTNEED);
#endif
        a->retained = 0;
    }
    struct cajeta_arena_pooled* node = malloc(sizeof(*node));
    if (!node) {
        // Can't track it — unmap rather than leak the reservation.
#if defined(_WIN32)
        VirtualFree(a->base, 0, MEM_RELEASE);
#else
        munmap(a->base, CAJETA_ARENA_RESERVE);
#endif
        a->base = NULL;
        return;
    }
    node->base = a->base;
    node->retained = a->retained;
    node->committed = a->committed;
    pthread_mutex_lock(&__cajeta_arena_pool_mu);
    node->next = __cajeta_arena_pool;
    __cajeta_arena_pool = node;
    pthread_mutex_unlock(&__cajeta_arena_pool_mu);
    a->base = NULL;
    a->bump = 0;
    a->retained = 0;
    a->count = 0;
    a->committed = 0;
}

static inline size_t __cajeta_arena_align8(size_t n) {
    return (n + 7u) & ~(size_t) 7u;
}

// __attribute__((malloc)): each bump is a fresh, disjoint region, so it aliases
// nothing live. Without it LLVM reloads arena array bases — a ~2.3x regression.
__attribute__((malloc)) static inline void* __cajeta_arena_bump(uint64_t size) {
    cajeta_arena* a = __cajeta_arena_ptr();
    if (!a->base) __cajeta_arena_init(a);
    size_t n = __cajeta_arena_align8((size_t) size);
    if (a->bump + n > CAJETA_ARENA_RESERVE) {
        fprintf(stderr, "cajeta: arena exhausted (request=%zu, reserve=%zu)\n",
                (size_t) size, (size_t) CAJETA_ARENA_RESERVE);
        abort();
    }
    unsigned char* p = a->base + a->bump;
    a->bump += n;
    if (a->bump > a->retained) {
        a->retained = a->bump;
    }
#if defined(_WIN32)
    // Commit the page(s) the new [p, p+n) region spans before returning it.
    if (a->bump > a->committed) {
        size_t pg = 4096;                                   // x64 Windows page
        size_t need = (a->bump + (pg - 1)) & ~(pg - 1);
        if (need > CAJETA_ARENA_RESERVE) need = CAJETA_ARENA_RESERVE;
        if (!VirtualAlloc(a->base + a->committed,
                          need - a->committed,
                          MEM_COMMIT, PAGE_READWRITE)) {
            fprintf(stderr, "cajeta: arena VirtualAlloc(MEM_COMMIT) failed\n");
            abort();
        }
        a->committed = need;
    }
#endif
    return p;
}

// Zeroed arena alloc: a reset reuses memory, so the bytes may be dirty.
__attribute__((malloc)) void* __cajeta_arena_alloc(uint64_t size) {
    void* p = __cajeta_arena_bump(size);
    memset(p, 0, __cajeta_arena_align8((size_t) size));
    return p;
}

// Uninitialized arena alloc (the caller overwrites every byte).
__attribute__((malloc)) void* __cajeta_arena_alloc_uninit(uint64_t size) {
    return __cajeta_arena_bump(size);
}

// Arena variant of __cajeta_new_array_header: the same byte layout, so every
// array reader works, but not live-set tracked — the scope-exit reset takes it.
__attribute__((malloc)) void* __cajeta_new_array_header_arena(uint64_t header_size, uint64_t elem_size, uint64_t count) {
    if (elem_size != 0 && count > (UINT64_MAX - header_size) / elem_size) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header_arena overflow (header=%llu elem=%llu count=%llu)\n",
                (unsigned long long) header_size,
                (unsigned long long) elem_size,
                (unsigned long long) count);
        abort();
    }
    uint64_t total = header_size + count * elem_size;
    if (total == 0) {
        return NULL;
    }
    void* hdr = __cajeta_arena_alloc((uint64_t) total);   // zeroed
    *((int64_t*) hdr) = (int64_t) count;
    __cajeta_arena_ptr()->count++;   // reclaimed (and drop-counted) at the next scope reset
    return hdr;
}

// Frame-arena membership probe: a pointer inside this context's reservation is
// frame-transient, so it can never back a Shared stake. Stale pointers count too.
int __cajeta_arena_owns(const void* p) {
    cajeta_arena* a = __cajeta_arena_ptr();
    return a->base
        && (const unsigned char*) p >= a->base
        && (const unsigned char*) p < a->base + CAJETA_ARENA_RESERVE;
}

// Capture the bump offset AND the live array count for a later reset: count in
// the high bits, bump in the low 40 (the reserve is 4 GiB). Opaque to codegen.
uint64_t __cajeta_arena_mark(void) {
    cajeta_arena* a = __cajeta_arena_ptr();
    return ((uint64_t) a->count << 40) | (uint64_t) a->bump;
}

// O(1) reclaim: restore the bump to `mark`, abandoning everything above it. Trim
// backstop: pages well past the new mark are handed back to the OS.
void __cajeta_arena_reset(uint64_t mark) {
    cajeta_arena* a = __cajeta_arena_ptr();
    size_t m       = (size_t) (mark & (((uint64_t) 1 << 40) - 1));
    size_t count_m = (size_t) (mark >> 40);
    // The arrays reclaimed here used to tick the drop count one free at a time.
    if (a->count > count_m) {
        __cajeta_drop_count_add((int64_t) (a->count - count_m));
    }
    a->count = count_m;
    a->bump = m;
    if (a->base
            && a->retained > m + CAJETA_ARENA_TRIM_THRESHOLD) {
#if defined(_WIN32)
        size_t pg = 4096;
#else
        size_t pg = (size_t) sysconf(_SC_PAGESIZE);
        if (pg == 0) pg = 4096;
#endif
        size_t from = (m + pg - 1) & ~(pg - 1);                       // round up
        size_t to   = (a->retained + pg - 1) & ~(pg - 1);
        if (to > from) {
#if defined(_WIN32)
            // Hand the pages back to the OS; a later bump re-commits them.
            VirtualFree(a->base + from, to - from, MEM_DECOMMIT);
            if (a->committed > from) a->committed = from;
#else
            madvise(a->base + from, to - from, MADV_DONTNEED);
#endif
        }
        a->retained = m;
    }
}

// Test/introspection: current bytes in use, and high-water retained bytes.
int64_t __cajeta_arena_bytes(void)    { return (int64_t) __cajeta_arena_ptr()->bump; }
int64_t __cajeta_arena_retained(void) { return (int64_t) __cajeta_arena_ptr()->retained; }

// cajeta.lang.String wrapper layout (slices plan 6.2.2): the storage is the
// 16-byte tagged Utf8 core, with `mode` collapsed into the tag.
//   len <= 12   Inline — the text lives in `data`; self-contained.
//   len >  12   pointer form: data overlays {i32 off, char* base}, where base is
//               a ROOT CajetaArray header and the text sits at base + 8 + off.
//               Tag bits, all of which apply only to this form:
//     CAJ_STR_SHARED_BIT  the wrapper holds one rc stake on base
//     CAJ_STR_BORROW_BIT  stakeless view of a heap root; drop must not touch it
//     CAJ_STR_STATIC_BIT  static root (literals and views of them); no rc ever
//     no bits             OWNED sole root; drop frees via the owner-drop seam
#define CAJ_STR_LEN_MASK   0x1FFFFFFF
#define CAJ_STR_SHARED_BIT ((int32_t) 1 << 31)
#define CAJ_STR_BORROW_BIT ((int32_t) 1 << 30)
#define CAJ_STR_STATIC_BIT ((int32_t) 1 << 29)
#define CAJ_STR_INLINE_CAP 12

typedef struct {
    void*   vtable;
    int32_t lenTag;
    char    data[12];     // Inline text, or the {off, base} pointer overlay
    int32_t cachedCpLength;
} cajeta_string_layout;

static inline int32_t caj_str_len(const cajeta_string_layout* s) {
    return s->lenTag & CAJ_STR_LEN_MASK;
}
static inline int caj_str_is_pointer(const cajeta_string_layout* s) {
    return caj_str_len(s) > CAJ_STR_INLINE_CAP;
}
static inline int32_t caj_str_off(const cajeta_string_layout* s) {
    int32_t o;
    memcpy(&o, s->data, 4);
    return o;
}
static inline char* caj_str_base(const cajeta_string_layout* s) {
    char* b;
    memcpy(&b, s->data + 4, 8);
    return b;
}
static inline const char* caj_str_ptr(const cajeta_string_layout* s) {
    if (!caj_str_is_pointer(s)) return s->data;
    return caj_str_base(s) + 8 + caj_str_off(s);
}
static inline void caj_str_set_inline(cajeta_string_layout* s,
        const char* src, int32_t len) {
    s->lenTag = len;
    memset(s->data, 0, sizeof s->data);
    if (len > 0) memcpy(s->data, src, (size_t) len);
}
static inline void caj_str_set_window(cajeta_string_layout* s,
        int32_t lenTag, int32_t off, void* base) {
    s->lenTag = lenTag;
    memcpy(s->data, &off, 4);
    memcpy(s->data + 4, &base, 8);
}
// Build a fresh owned root (count word + text + NUL); returns the header.
static inline void* caj_str_new_root(const char* src, int32_t len) {
    void* buf = __cajeta_new_array_header(8, 1, (uint64_t) len + 1);
    *((int64_t*) buf) = len;
    if (len > 0) memcpy((char*) buf + 8, src, (size_t) len);
    ((char*) buf)[8 + len] = 0;
    return buf;
}

// Mode-aware drop for a String value, assuming the CALLER already won this
// wrapper's live-set claim. Runs the tag dispatch and frees the wrapper.
void __cajeta_string_drop_claimed(void* s) {
    cajeta_string_layout* str = (cajeta_string_layout*) s;
    int32_t tag = str->lenTag;
    char* base = caj_str_is_pointer(str) ? caj_str_base(str) : NULL;
    // Tag dispatch (slice-spec §8.2): Inline is self-contained, BORROW holds no
    // stake, STATIC is never freed, SHARED releases one, OWNED frees the root.
    if (base != NULL && !(tag & (CAJ_STR_BORROW_BIT | CAJ_STR_STATIC_BIT))) {
        if (tag & CAJ_STR_SHARED_BIT) {
            int __cajeta_shared_release(void* b);
            if (__cajeta_shared_release(base)) {
                __cajeta_poison_buffer(base);
                free(base);
            }
        } else {
            __cajeta_free_array(base);
        }
    }
    __cajeta_poison_buffer(s);
    free(s);
}

// Drop-fn entry point (void(*)(void*)): claims the wrapper out of the live set,
// so it is idempotent and a no-op on an untracked static literal wrapper.
void __cajeta_string_drop(void* s) {
    if (!s) return;
    if (!__cajeta_live_set_claim(s)) return;   // static wrapper / already freed
    __cajeta_string_drop_claimed(s);
}

// Drop dispatcher for function-typed locals: reads the closure record's drop_fn
// and calls it when non-null, so a constant record or a null pointer is a no-op.
struct cajeta_closure_record {
    void* fn;
    void* captures;
    void (*drop_fn)(void*);
};

void __cajeta_closure_drop(void* p) {
    if (!p) return;
    struct cajeta_closure_record* c = (struct cajeta_closure_record*) p;
    if (c->drop_fn) c->drop_fn(p);
}

// Frees a whole around-advice chain (each record's `captures` is the next).
void __cajeta_closure_chain_free(void* p) {
    struct cajeta_closure_record* c = (struct cajeta_closure_record*) p;
    while (c) {
        struct cajeta_closure_record* next = (struct cajeta_closure_record*) c->captures;
        __cajeta_free(c);
        c = next;
    }
}

// --- Threading sync primitives: Lock --------------------------------------
