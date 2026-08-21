// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// ============================================================================
// Poison-on-free (CompilerModes.md § --poison-free).
//
// When enabled, every heap block is memset to a sentinel byte right before
// the actual free() runs. The pattern (0xDB) is unlikely to read as a valid
// pointer or sensible integer, so a use-after-free against poisoned memory
// either traps (deref of 0xDBDBDBDB...) or surfaces obviously wrong data.
//
// Flag wiring: __cajeta_set_poison_free(int) is called from the JIT-init
// hook (CajetaJit::compile) with the active CompilerFlags.poisonFree value,
// or — for binary compilation — from a global ctor emitted by the compiler.
// Default is off (0) so existing tests that don't go through the JIT hook
// keep prior behavior.
//
// Chunk-size source: malloc_usable_size(3) returns the actual allocated
// chunk (≥ requested size, may be larger due to malloc rounding). Poisoning
// the whole chunk is harmless — the over-fill stays inside the chunk and
// gets reclaimed at free time anyway.
// ============================================================================
static int __cajeta_poison_free_enabled = 0;

void __cajeta_set_poison_free(int enabled) {
    __cajeta_poison_free_enabled = enabled ? 1 : 0;
}

int __cajeta_get_poison_free(void) {
    return __cajeta_poison_free_enabled;
}

// Sentinel-fill a buffer with 0xDB up to its allocator-tracked chunk size.
// No-op when the flag is off or ptr is NULL. Kept as a separate symbol so
// tests can exercise the poison logic without dipping into use-after-free
// undefined behavior (the buffer is still valid after this call until free
// runs).
void __cajeta_poison_buffer(void* ptr) {
    if (!__cajeta_poison_free_enabled) return;
    if (!ptr) return;
    size_t n = cajeta_malloc_usable_size(ptr);
    if (n == 0) return;
    memset(ptr, 0xDB, n);
}

// ============================================================================
// Debug safepoints (debugger CP2+). When the compiler is run with
// --debug-info, statement-boundary codegen emits a call to
// __cajeta_dbg_safepoint(loc_id) before each statement. For CP2 this just
// counts hits so the TDD harness can verify emission/execution; CP3 adds
// breakpoint-arming + fiber park. As with the poison-free flag, the
// embedded-bitcode copy (called by JIT'd user code) and the native-object
// copy (callable from host C++ in tests) each keep their own static counter
// — read whichever copy you exercised.
// ============================================================================
static long __cajeta_dbg_safepoint_total = 0;

// Monotonic fiber-id source (CP3). Defined here so __cajeta_task_run (further
// down) can bump it at fiber creation; read back via dbg_id on the fiber.
long __cajeta_dbg_fiber_id_counter = 0;

// ── Debugger CP6f-2: live-fiber registry ──────────────────────────────────
// A snapshot of currently-live fibers so the DAP `threads`/fibers view can
// enumerate them: a fiber registers at __cajeta_task_run and unregisters when
// the carrier frees it (state == DONE). Stores opaque fiber handles; the
// per-fiber accessors further down cast them back to struct cajeta_fiber* so
// the host's native runtime copy can read a fiber the JIT copy registered.
//
// Guarded by its own mutex with a strict one-way nesting: the spawn path locks
// task→reg, while the carrier-free and host-enumeration paths lock reg only —
// nothing ever locks reg then task, so there's no deadlock against the task
// mutex. Enumeration happens on the debugger thread while a breakpoint is
// parked (the carrier holds no reg lock then), so it never blocks the program.
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
        // Order-preserving removal: shift the tail down so the view stays in
        // stable spawn order across stops (no swap-with-last hole).
        for (int j = i + 1; j < __cajeta_dbg_fiber_reg_count; j++) {
            __cajeta_dbg_fiber_reg[j - 1] = __cajeta_dbg_fiber_reg[j];
        }
        __cajeta_dbg_fiber_reg_count--;
        break;
    }
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
}

// Number of live fibers (debugger thread reads this while the program is
// parked). Excludes the program/main thread, which the DAP layer reports as a
// synthetic id-0 thread.
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

// CP6f-2d unit 1: atomic registry snapshot. Copies up to `max` live-fiber
// handles (spawn order) into `out` under a SINGLE lock hold, and returns the
// total live count at that instant. The single critical section is the whole
// point: the previous enumeration (count() then a loop of at(i), releasing the
// lock between calls) is a TOCTOU that races concurrent register/unregister on
// the still-running carriers (the program is not yet stopped-the-world — see
// docs/specs/carrier-quiesce-spec.md). Returning the full count (which may
// exceed `max`) lets the caller grow its buffer and re-snapshot; passing
// out==NULL or max<=0 just reads the count.
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

// === CP6f-2d unit 2: debug-only stop coordinator ==========================
// Process-global rendezvous for cross-carrier stop-the-world (spec §2.1). A
// breakpoint/exception sets `__cajeta_stop_requested`; every carrier observes
// it at its next safepoint / scheduler hand-off (wired in units 3-6) and parks
// via __cajeta_stop_park; the debugger waits on the convergence barrier
// (__cajeta_stop_wait_converged) until parked==expected or a bounded timeout,
// then resumes all with __cajeta_stop_clear. Zero cost off-path: the hot-path
// check is __cajeta_stop_is_requested(), a single relaxed-atomic load (§4.2) —
// the slow path (mutex/condvar) is touched only once a stop is in flight.
//
// Lock order (§4.1, acyclic): __cajeta_stop_mu is a LEAF — code holding it
// never takes the registry or carrier/deque mutexes. The flag is read/written
// with relaxed atomics so the safepoint fast path needs no lock; the
// mutex+condvars serialize the count transitions and the park/resume waits.
static pthread_mutex_t __cajeta_stop_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __cajeta_stop_resume_cv    = PTHREAD_COND_INITIALIZER; // carriers wait here
static pthread_cond_t  __cajeta_stop_converged_cv = PTHREAD_COND_INITIALIZER; // debugger waits here
static int      __cajeta_stop_requested = 0;   // 0/1 flag — hot-path relaxed read
static unsigned __cajeta_stop_generation = 0;  // bumped on every request and clear
static int      __cajeta_stop_parked = 0;      // carriers currently parked
static int      __cajeta_stop_expected = 0;    // carriers that must park before inspect

// Hot path. A single relaxed load — gated upstream by the safepoint/session
// guard, so when nothing is armed this is the only added instruction (§4.2).
int __cajeta_stop_is_requested(void) {
    return __atomic_load_n(&__cajeta_stop_requested, __ATOMIC_RELAXED);
}

// Open a stop round. Returns 1 iff this caller flipped 0->1 (the *primary*,
// §4.3); 0 if a stop was already in progress (this caller is a secondary).
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

void __cajeta_stop_set_expected(int n) {
    pthread_mutex_lock(&__cajeta_stop_mu);
    __cajeta_stop_expected = n;
    pthread_cond_broadcast(&__cajeta_stop_converged_cv);  // a lowered bar may already be met
    pthread_mutex_unlock(&__cajeta_stop_mu);
}

// A carrier observed the stop and parks here: count itself, wake the debugger's
// convergence wait, then block until THIS round is cleared. If the round was
// already cleared between the carrier's flag read and acquiring the lock, this
// returns immediately (never parks into a resumed world) — the generation guard
// also releases it promptly if a *new* round opens.
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

// Debugger-side barrier. Block until parked>=expected, or until timeout_ns
// elapses (<=0 waits indefinitely). Returns the count still NOT parked (0 ==
// fully quiesced) so the caller can flag un-quiesced carriers (spec §2.3, §5.4)
// rather than hang on a carrier stuck in a native call.
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

// CP3: a settable safepoint handler. When the in-process debugger is attached
// it installs one (via the JIT symbol so the embedded-bitcode copy's pointer
// is set); the handler decides — based on the host-side armed set — whether to
// park the calling thread. NULL by default, so a debug-info build with no
// debugger attached just counts (CP2 behavior). The fiber id is resolved via
// __cajeta_dbg_current_fiber_id, which is defined later (after the
// __cajeta_current_fiber TLS); forward-declared here.
// CP5: per-fiber debug frame chain. When --debug-info is on, codegen emits a
// __cajeta_dbg_frame_enter at each method prologue, __cajeta_dbg_frame_leave on
// every return path, and __cajeta_dbg_local at each named local/parameter
// alloca. The chain (one node per live call frame, innermost at the head)
// lets the debugger walk the stack and read locals when a safepoint parks.
//
// Like scope_top/drop_top, the head lives per-fiber (a single __thread would
// alias across fiber switches on the same carrier); the selector
// __cajeta_dbg_top_ptr (defined near __cajeta_scope_top_ptr) picks the running
// fiber's slot or the main/program-thread TLS. The `name`/`type` strings are
// codegen-emitted constants (valid for the program's lifetime); `addr` is the
// local's slot alloca. CAVEAT: frame_leave fires only on normal return paths,
// not exception unwinding — a throw across a debug frame leaks its node (the
// breakpoint-inspect flow doesn't throw; revisit if stepping through throws).
#define CAJETA_DBG_MAX_LOCALS 64
struct cajeta_dbg_local {
    const char* name;
    const char* type;   // cajeta canonical type name (e.g. "int32", "demo.Foo")
    void* addr;          // the local's slot (primitives: holds the value;
                         // objects: holds the heap pointer)
    // CP7-1b: memory facets for ownership/allocation visualization. Two
    // orthogonal bytes mirroring cajeta::dbg::AllocClass / OwnershipRole
    // (see src/cajeta/dbg/MemoryFacets.h): alloc = where the value lives
    // (0 unknown / 1 stack / 2 heap / 3 shared), ownership = who is
    // responsible (0 unknown / 1 owner / 2 borrow / 3 moved-out). Carried
    // here as plain bytes so this C ABI stays decoupled from the C++ enum;
    // the host reads them back through the accessors below and maps them.
    uint8_t alloc;
    uint8_t ownership;
    // CP7-1c: the owner's drop-chain entry (a struct cajeta_drop_entry*, base
    // or debug shape — its `active` flag lives at the same offset in both), or
    // NULL for a non-owner. The host reads `active` LIVE at a stop to derive
    // lifetime state (active => about-to-drop, cleared => moved-out at runtime).
    // A raw void* so this struct stays above the cajeta_drop_entry definition.
    void* drop_entry;
};
struct cajeta_dbg_frame {
    const char* func;          // cajeta-mangled enclosing function name
    int32_t current_loc;       // loc_id of the last safepoint hit in this frame
    int nlocals;
    struct cajeta_dbg_local locals[CAJETA_DBG_MAX_LOCALS];
    struct cajeta_dbg_frame* prev;
    // resident-debug-server 9.1: the chain slot this frame was pushed onto.
    // leave() unlinks the node from ITS OWN chain — a method that enters
    // under one fiber context and leaves under another (parallel shares,
    // scheduler hand-offs) previously popped the WRONG chain, eroding
    // main's chain until pending steps could never land.
    struct cajeta_dbg_frame** owner;
};

// Selector for the current dbg frame chain head — mirrors __cajeta_scope_top_ptr
// (fiber vs main TLS). Defined further down where __cajeta_current_fiber is in
// scope; forward-declared here so the enter/leave/local helpers can use it.
struct cajeta_dbg_frame** __cajeta_dbg_top_ptr(void);

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

// Node-paired leave (9.1): unlink EXACTLY the frame this call's matching
// enter pushed, from the chain it was pushed onto — immune to fiber-context
// changes between enter and leave. Not found on its owner chain (already
// unlinked by an unwind path): leak rather than corrupt.
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

// --- diagnostic-exceptions Unit 3: line-info shadow stack --------------------
//
// A lightweight per-thread stack mirroring the active Cajeta call frames, used
// to attach exact `file:line` to captured stack traces WITHOUT DWARF. Codegen
// (gated `--line-info`) emits `__cajeta_line_enter(desc)` at each method
// prologue, `__cajeta_line_mark(line)` at each statement boundary, and
// `__cajeta_line_leave()` on every normal return path. `desc` is a codegen-
// emitted `{typeName, methodName, fileName}` constant (program lifetime). On a
// throw the runtime snapshots this stack into the trace side table (see
// `__cajeta_trace_record`), and `getStackTrace()` resolves each frame from it.
//
// Unlike the debugger frame chain above, this never mallocs (fixed array) and
// stores the line directly (no compiler-side loc table), so it resolves fully
// in an AOT exe. `leave` fires only on normal returns; an exception unwind is
// handled by restoring `__cajeta_shadow_top` to the catching try-frame's
// watermark in `__cajeta_throw` (see cajeta_rt_io.c) — so a throw-across-frames
// leaves no stale entries.
//
// cajeta-profiler Unit 2: the stack is PER FIBER, not per carrier thread. A
// carrier hosts many fibers on one OS thread, so a single `__thread` stack
// aliased across every fiber it ran — frames from different fibers interleaved
// into one stack and a yield left stale entries behind. The selector
// __cajeta_shadow_ptr picks the running fiber's slot or the program thread's
// TLS, exactly as __cajeta_dbg_top_ptr does for the debug frame chain.
//
// The slot is an inline fixed array rather than a lazily-allocated pointer, so
// this keeps its never-mallocs property on the enter/mark/leave hot path. It
// costs CAJETA_SHADOW_MAX * sizeof(CajetaShadowFrame) = 8 KB per fiber against
// a CAJETA_FIBER_STACK_SIZE of 1 MB — 0.8% of what a fiber already reserves.
// CajetaFrameDesc / CajetaShadowFrame / CajetaProfSample and their bounds live
// in the profiler ABI header, so the trace writer's transform can be driven by a
// synthetic sample array in CI without dragging in the whole runtime.
#include "cajeta_prof_abi.h"


// One shadow stack. The program thread owns the __thread instance below; each
// fiber owns one inline in `struct cajeta_fiber`.
typedef struct {
    CajetaShadowFrame frames[CAJETA_SHADOW_MAX];
    int32_t top;
} CajetaShadowStack;

// Program/main-thread slot — used by any thread that is not running a fiber
// (the JIT entry runs on a plain bg thread, not a carrier fiber).
static __thread CajetaShadowStack __cajeta_main_shadow;

// Selector for the live shadow stack, mirroring __cajeta_scope_top_ptr /
// __cajeta_dbg_top_ptr. Defined in cajeta_rt_concurrent_exec.c, where
// __cajeta_current_fiber and struct cajeta_fiber are in scope.
CajetaShadowStack* __cajeta_shadow_ptr(void);

void __cajeta_line_enter(const void* desc) {
    CajetaShadowStack* s = __cajeta_shadow_ptr();
    int32_t t = s->top;
    if (t >= 0 && t < CAJETA_SHADOW_MAX) {
        s->frames[t].desc = (const CajetaFrameDesc*) desc;
        s->frames[t].line = 0;
    }
    s->top = t + 1;   // count past the cap so leave stays balanced
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
// ── cajeta-profiler Unit 3: live-thread registry ──────────────────────────
// Publishes each live PROGRAM THREAD's shadow stack so a sampler thread can
// read a stack it does not own. A thread registers at its entry (§2.7 wants it
// sampled from creation, so this is not lazy — a lazy check would also put a
// branch on the enter/mark/leave hot path) and unregisters before it exits.
//
// FIBERS ARE DELIBERATELY ABSENT. The debugger's live-fiber registry above
// already enumerates them under a single-lock snapshot, and since Unit 2 every
// fiber carries its shadow stack inline, so a fiber handle is all a sampler
// needs. A second registry over the same fibers would drift from that one.
//
// The mutex is taken on register/unregister/snapshot — never on the sampled
// thread's enter/mark/leave path, which touches no shared state at all (3.3.a).
static pthread_mutex_t __cajeta_prof_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static CajetaShadowStack** __cajeta_prof_threads = NULL;
static int __cajeta_prof_thread_n = 0;
static int __cajeta_prof_thread_cap = 0;

// This thread's own shadow-stack handle — always the PROGRAM-thread slot, never
// a fiber's: fibers are enumerated through the fiber registry, and a carrier
// asked for "its" stack means the carrier's, not whichever fiber it is hosting.
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
        // Order-preserving removal, matching the fiber registry: a sampler's
        // view stays in registration order rather than shuffling on every exit.
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

// Copy up to `max` live handles under a SINGLE lock hold and return the live
// count at that instant. Same rationale as __cajeta_dbg_fiber_snapshot:
// count() followed by a loop of at(i) is a TOCTOU against threads registering
// and exiting concurrently. Returning the full count (which may exceed `max`)
// lets a caller grow its buffer and re-snapshot.
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

// Snapshot the frames behind a shadow stack from ANOTHER thread — the sampler's
// entry point. `handle` is a CajetaShadowStack*, full stop: a thread handle from
// __cajeta_prof_thread_snapshot already is one, and a FIBER handle must be put
// through __cajeta_dbg_fiber_shadow_of first.
//
// This comment used to claim a fiber's shadow stack was the first member of
// struct cajeta_fiber, so both handles resolved to the same shape. That was
// false — `shadow` sits behind a ucontext_t and a dozen pointers — and it was
// load-bearing, because it was the justification for casting a fiber handle
// straight across. Every fiber then sampled as empty. Nothing failed; the fiber
// lane was just never in any trace.
//
// `truncated` (spec §2.8) reports that the source stack was DEEPER than
// capacity, so a caller never reads a capped stack as a complete one. The signal
// costs nothing: __cajeta_line_enter deliberately counts `top` past the cap to
// keep `leave` balanced, so an over-cap depth is already recorded.
//
// Lock-free by construction: it reads a live stack that its owner is still
// mutating. A sample can therefore tear — a frame written while it is copied —
// which is inherent to sampling a running thread and is why §11 verifies sample
// plausibility rather than trusting it. Never blocks the sampled thread.
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

// ── cajeta-profiler Unit 4: the sampler ───────────────────────────────────
// A dedicated thread walks the Unit 3 thread registry and the debugger's
// live-fiber registry on an interval, copying each live shadow stack into a
// ring. Nothing is added to the sampled program's path: arming starts a thread,
// it does not emit a probe. That is what makes §2.2 true — a binary built with
// default flags is profilable with no rebuild, because the frames the sampler
// reads are the line-info probes every build already carries. Unarmed cost is
// therefore exactly zero (§2.9, §13.4): no thread, no allocation, no branch.
//
// The ring is fixed-capacity and the producer is the sampler thread alone, so
// there is no allocation on the sampling path. On overflow it DROPS and counts
// the drop rather than blocking or growing: the alternative is a sampler that
// perturbs the program it measures, and a silent drop is indistinguishable from
// a correct run from outside — hence the counter, which the trace reports.
// Defined in cajeta_rt_concurrent_exec.c, later in this TU — same forward-
// declaration pattern as __cajeta_dbg_top_ptr / __cajeta_shadow_ptr above.
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

// 4.2.e: codegen registers that line-info probes were emitted, via a global
// ctor — NOT a weak extern. See cajeta_rt_core.c's loc-table note above: the
// runtime bitcode is linked into each module long before codegen emits its
// definition, so a weak default here and a strong one there collide in one
// module and LLVM silently renames the second.
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

// Copy one stack into the ring. Producer-only; the sampler thread is the sole
// writer, so head moves without a CAS.
static void __cajeta_prof_push(void* owner, int32_t owner_kind) {
    int32_t trunc = 0;
    CajetaShadowFrame tmp[CAJETA_PROF_MAX_FRAMES];
    // A THREAD handle IS its shadow stack (the registry stores
    // &__cajeta_main_shadow). A FIBER handle is a struct cajeta_fiber*, whose
    // shadow stack is a member well inside it — resolve it rather than casting
    // across. Getting this wrong is silent: the bogus depth reads non-positive,
    // the sample is dropped as "idle", and the fiber lane is simply absent from
    // every trace with nothing anywhere reporting a problem.
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
    // The sampler does NOT register itself in the thread registry: it would
    // sample its own stack, which is not the program's work.
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
                "cajeta.profiler: refusing to arm — this binary was built with "
                "--line-info=off, so there are no frames to sample. Rebuild "
                "without it (line-info is on by default).\n");
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
    __cajeta_prof_head = __cajeta_prof_tail = 0;
    __cajeta_prof_stop = 0;
    // The arming thread is the program thread; register it here so §2.1's
    // "every host thread" holds without a lazy check on the enter/mark/leave
    // hot path (plan 3.2.d).
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
    // The ring is NOT freed here: Unit 5's writer drains it on the way out, and
    // freeing under it would lose the tail of every run that ends normally.
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

// Print the LIVE shadow stack — the frames on this thread right now, innermost
// first — as `at Type.method(File.cajeta:NN)` lines to `fd` (1 stdout, 2 stderr).
//
// The shadow stack already resolves a *captured* trace (a throwable's, via
// __cajeta_print_trace), which is the semantic alternative to DWARF across the
// whole product matrix: it works identically in the JIT, in an AOT binary, and
// on device targets, none of which carry debug sections. What was missing is the
// live view a DEBUGGER needs. An external debugger stopped at a breakpoint has a
// native backtrace of mangled symbols and nothing else; from gdb,
//
//     (gdb) call (void) __cajeta_print_stack(2)
//
// renders the Cajeta call stack with source files and line numbers, with no
// debug info in the binary. Also the natural source for a `cajeta dap`
// stackTrace request and for a panic handler.
//
// Reads only thread-local state written by __cajeta_line_enter/mark/leave, so it
// is safe to call from a stopped thread. No allocation, no locks.
//
// `used, retain` on this and the accessors below: NOTHING in generated code calls
// them — a debugger does, from outside — so the IR-level DCE (lean link) and the
// linker's --gc-sections would both drop them, and the symbol would simply not be
// in the binary when you need it. `used` pins the definition through GlobalDCE
// (@llvm.used); `retain` marks the section SHF_GNU_RETAIN so the linker keeps it
// too. Cost is a few hundred bytes.
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
// Field accessors rather than a struct return, so a debugger — or any consumer
// that cannot see this file's types without debug info — can walk frames
// through plain calls.
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

// ============================================================================
// The embedded location table (external-debug §3).
//
// The compiler's DbgLocTable maps loc_id -> {file, line, col, function}. It is
// a compiler-PROCESS global: `cajeta dap` can read it only because the DAP
// compiles and runs in one process. An external debugger attached to a built
// binary has no compiler, so under --debug-info=full codegen serializes the
// table into the binary as a constant array and emits a global ctor that
// registers it here. These accessors are then the debugger's whole view:
// loc_id -> source position, and (file, line) -> the ids to arm.
//
// Registration rather than a weak extern: the table's definition is emitted at
// end of codegen, but the runtime bitcode is linked into each module long
// before that, so a weak default here and a strong definition there would
// collide in one module (LLVM renames the second, silently). A ctor call is
// the same shape the XPU kernel registry already uses, and it behaves
// identically under LLJIT and a native link.
//
// A `line` or `off` build registers nothing: the table stays empty and every
// accessor answers benignly (§3.1.4). `used, retain` because nothing in
// generated code calls these — DCE and --gc-sections would otherwise drop
// them, which is exactly how __cajeta_print_stack was lost on its first cut.
// ============================================================================
typedef struct {
    const char* file;
    int32_t     line;
    int32_t     col;
    const char* func;
} CajetaDbgLocEntry;

static const CajetaDbgLocEntry* __cajeta_dbg_loc_entries = 0;
static int32_t __cajeta_dbg_loc_n = 0;

// Called from the ctor codegen emits under --debug-info=full. A null/empty
// registration CLEARS the table (an `off`/`line` binary registers nothing at
// all, and this keeps "no table" reachable for tests).
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

// The ids a line breakpoint on (file, line) must arm. `file` matches by suffix
// on a path boundary, so a debugger may pass either the table's stored path
// ("cajeta/lang/Guid.cajeta") or just the basename ("Guid.cajeta"). Writes up
// to `max` ids into `out` and returns the number written; pass out=0 to count.
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
        // Suffix must start at a path boundary: "Guid.cajeta" must not match
        // "MyGuid.cajeta".
        if (elen > flen && e->file[elen - flen - 1] != '/') continue;
        if (out && found < max) out[found] = i;
        found++;
    }
    return found;
}

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

// Stateless host-side accessors. The frame chain is built by the embedded
// bitcode copy of this runtime (JIT'd code calls frame_enter/local); the host
// reads it through the NATIVE copy. Pure pointer arithmetic on a passed-in
// void* means both copies resolve a chain node identically, so the host can
// dereference a pointer the JIT side produced. Used by DebugVars::walkFrames.
//
// external-debug §4: `used, retain` on all of them. In-process (the DAP) they are
// called from C++ and survive; in an AOT binary NOTHING calls them, so DCE and
// --gc-sections drop them — leaving an external debugger unable to read the very
// locals the program is busy recording.

// The chain head, for a debugger that has no frame pointer to start from. The DAP
// is HANDED frame_top by the safepoint handler; gdb is not, so it needs its own
// way in. Sound for an AOT binary, where the runtime is a single copy and there is
// no JIT/native TLS split.
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
// CP7-1b: the two memory facets. Out-of-range reads back 0 (== Unknown), the
// same neutral fallback codegen uses when a facet isn't statically known.
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

// CP5: the handler now also receives the current dbg frame-chain head so the
// host can walk frames + read locals without a TLS lookup (the chain lives in
// the bitcode copy's TLS, unreachable from the host's native copy).
typedef void (*cajeta_dbg_handler_fn)(int32_t loc_id, int fiber_id,
                                      void* frame_top);
static cajeta_dbg_handler_fn __cajeta_dbg_handler = NULL;
int __cajeta_dbg_current_fiber_id(void);

void __cajeta_dbg_set_safepoint_handler(cajeta_dbg_handler_fn fn) {
    __cajeta_dbg_handler = fn;
}

// jupyter-kernel U6 (spec 5.1) — the notebook interrupt.
//
// A safepoint is the one place a running cell is known to be between
// statements, which is exactly where it is safe to stop it. The flag lives
// HERE, beside its only reader, so the off-path cost of interruptibility is a
// single relaxed load per safepoint rather than a call into another
// translation unit.
//
// Set from ANOTHER thread — the kernel's control channel answers while a cell
// is running, which is the whole point — so it is atomic. The unwind itself
// happens on the running thread, at the safepoint, never on the setter's.
//
// NOT static, and that is load-bearing rather than sloppy. Under the JIT's
// partitioned/lazy materialization an `internal` global referenced from
// functions that land in different partitions can be DUPLICATED: the setter
// writes one copy and the safepoint reads another, forever. This TU already
// carries that scar — see `__cajeta_dbg_exception_handler` below, which was
// made external for exactly this reason after the exception handler "never
// fired" under `cajeta dap`. As a static, this flag reproduced it precisely:
// safepoints ran, the request was set, and the loop ran to completion.
// External linkage gives one unified definition.
volatile int __cajeta_session_interrupt_flag = 0;

// Defined in cajeta_rt_session.c (later in this TU): unwinds to the session
// guard. Does not return when there is a guard frame to land in.
void __cajeta_session_interrupt_unwind(void);

void __cajeta_session_request_interrupt(void) {
    __atomic_store_n(&__cajeta_session_interrupt_flag, 1, __ATOMIC_RELAXED);
}

// Cleared before every cell. A flag left set by an interrupt that arrived
// with nothing running would otherwise kill the NEXT cell — the frontend
// would see a cell the user never interrupted die (spec 5.2).
void __cajeta_session_clear_interrupt(void) {
    __atomic_store_n(&__cajeta_session_interrupt_flag, 0, __ATOMIC_RELAXED);
}

int __cajeta_session_interrupt_pending(void) {
    return __atomic_load_n(&__cajeta_session_interrupt_flag, __ATOMIC_RELAXED);
}

// TEST SEAM (jupyter-kernel 6.1). Trip the interrupt at the Nth safepoint
// from now, deterministically, on the running thread.
//
// The threaded form of this test — run a cell, interrupt from another thread —
// can only aim at a cell that runs long enough to be aimed at, which means a
// loop, which means the repro carries a loop's worth of state into every
// question you ask of it. This lets a THREE-STATEMENT cell take an interrupt
// at a known safepoint, which is what separates "the unwind is wrong" from
// "the unwind is wrong in loops". Negative disarms; 0 is off.
int __cajeta_session_interrupt_countdown = -1;

void __cajeta_session_interrupt_arm_after(int32_t n) {
    __atomic_store_n(&__cajeta_session_interrupt_countdown, n, __ATOMIC_RELAXED);
}

void __cajeta_dbg_safepoint(int32_t loc_id) {
    __cajeta_dbg_safepoint_total++;
    // U6 — take the request rather than test it: whoever unwinds owns it, so
    // a second safepoint cannot be interrupted by the same request, and the
    // flag is already clear when the cell's error is reported.
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
    // Record the line we're at in the innermost frame so a multi-frame
    // stackTrace shows each frame at its current/call statement.
    struct cajeta_dbg_frame* top = *__cajeta_dbg_top_ptr();
    if (top) top->current_loc = loc_id;
    cajeta_dbg_handler_fn h = __cajeta_dbg_handler;
    if (h) h(loc_id, __cajeta_dbg_current_fiber_id(), top);
    // CP6f-2d: cross-carrier convergence (spec §2.2.2). The handler parks the
    // PRIMARY (the carrier whose loc was armed) inside the DebugController.
    // Every OTHER carrier, reaching its next safepoint while a stop is in
    // flight, parks here as a secondary so no fiber advances past a safepoint
    // while the world is stopped. Off-path this is a single relaxed load
    // (__cajeta_stop_is_requested, §4.2); only a real stop pays the park cost.
    // The primary returns here AFTER resume, by which point the flag is cleared
    // — so it never double-parks.
    if (__cajeta_stop_is_requested()) __cajeta_stop_park();
}

// CP6f-3: settable exception handler. When the in-process debugger attaches
// with exception breakpoints armed it installs one (via the JIT symbol, like
// the safepoint handler); __cajeta_throw calls it at the throw chokepoint —
// BEFORE the stack unwinds — so the throwing frame chain is still intact for
// inspection. NULL by default (throws proceed normally). The handler receives
// the thrown Throwable*, the current fiber id, and the dbg frame-chain head.
typedef void (*cajeta_dbg_exception_fn)(void* throwable, int fiber_id,
                                        void* frame_top);
// NOT static: __cajeta_throw (far away in this TU) reads this global. Under the
// JIT's partitioned/lazy materialization an `internal` global referenced across
// distant functions can end up duplicated (the setter writes one copy, the
// throw reads another) — observed as the exception handler never firing under
// `cajeta dap` while the adjacent safepoint handler worked. External linkage
// gives a single unified definition. (CP6f-3.)
cajeta_dbg_exception_fn __cajeta_dbg_exception_handler = NULL;

void __cajeta_dbg_set_exception_handler(cajeta_dbg_exception_fn fn) {
    __cajeta_dbg_exception_handler = fn;
}

long __cajeta_dbg_safepoint_count(void) {
    return __cajeta_dbg_safepoint_total;
}

// resident-debug-server 9.1: the PROGRAM thread's identity. Captured here
// because reset_safepoint_count runs on the program thread immediately
// before the entry is invoked (runJit and debug sessions alike, once per
// session). Carrier threads outside fiber context must NOT report fiber id
// 0 — that impersonated the program thread and let parallel-share
// safepoints steal pending steps (stepped F8 parked in stream internals).
static pthread_t __cajeta_dbg_program_thread;
static int __cajeta_dbg_program_thread_set = 0;

int __cajeta_dbg_on_program_thread(void) {
    return __cajeta_dbg_program_thread_set
        && pthread_equal(pthread_self(), __cajeta_dbg_program_thread);
}

void __cajeta_dbg_reset_safepoint_count(void) {
    __cajeta_dbg_safepoint_total = 0;
    // Fallback capture for the SAME-THREAD runner (runJit invokes the entry
    // on this very thread). Debug sessions re-mark from their own program
    // thread below — reset runs on the SETUP thread there and must not win.
    if (!__cajeta_dbg_program_thread_set) {
        __cajeta_dbg_program_thread = pthread_self();
        __cajeta_dbg_program_thread_set = 1;
    }
}

// resident-debug-server 9.1: called as the program thread's FIRST act, from
// whichever thread actually runs the entry (session program thread; also
// re-marks per session so sequential resident sessions stay correct).
__attribute__((used, retain))
void __cajeta_dbg_mark_program_thread(void) {
    __cajeta_dbg_program_thread = pthread_self();
    __cajeta_dbg_program_thread_set = 1;
}

// 9.1 chain-containment probe: is `node` on the chain headed at `top`? A
// pure pointer chase like __cajeta_dbg_frame_depth; the pending-step gate
// uses it so a candidate on a FOREIGN chain can never satisfy a step armed
// on the origin chain. Bounded against cycles.
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

// ============================================================================
// Live-allocation set (FieldOwnership.md § Solution B).
//
// The auto field-drop scheme is "try to drop all fields; if the address is
// already gone, no-op." That requires a quick "is this address still live?"
// answer at every drop site. The live-set tracks every heap allocation we
// hand out; drop dispatchers atomically remove-and-claim, so the first call
// wins and subsequent calls (auto-drop and chain pop racing on the same
// aliased field, e.g. ArrayStream.data and ArrayList.data) no-op safely.
//
// Implementation: single global open-addressed hash table, fixed power-of-2
// capacity, pointer-shifted hash. Cross-fiber correctness needs the global
// (allocations may be freed on a different carrier than they were made on).
// Capacity is fixed; no resize — at 75% load we warn and start leaking the
// excess (correctness-preserving — leaked entries just mean future double-
// frees on those addresses won't be caught).
// ============================================================================
#define CAJETA_LIVE_SET_CAPACITY (1 << 18)   // 256K slots; 2MB total. Sized for the
                                             // peak working set of a large owned-key
                                             // map (e.g. a 30K-entry HashMap<String,V>
                                             // holds ~60K live allocations — wrapper +
                                             // byte buffer per key — simultaneously).
#define CAJETA_LIVE_SET_LOAD_CAP ((CAJETA_LIVE_SET_CAPACITY * 3) / 4)
#define CAJETA_LIVE_SET_TOMBSTONE ((void*) 1)  // page 0 unmapped; safe sentinel

static void* __cajeta_live_set[CAJETA_LIVE_SET_CAPACITY];
static int __cajeta_live_set_count = 0;
static pthread_mutex_t __cajeta_live_set_mu = PTHREAD_MUTEX_INITIALIZER;

// Current live-object population. Test-only introspection (Cajeta.liveCount()):
// lets a JIT test assert that an owning container actually reclaimed its #-taken
// keys/values on drop (count shrinks) rather than leaking them.
int64_t __cajeta_live_set_population(void) {
    return (int64_t) __cajeta_live_set_count;
}

// Cumulative bytes ever requested from the heap allocator across all of cajeta's
// allocation entry points — the runtime-neutral "allocation intensity" metric the
// profile harness reads via Cajeta.allocatedBytes() (mirrors the competitors'
// counting allocator / Go MemStats / tracemalloc). Monotonic, never reset; the
// harness samples a before/after delta around a benchmark. A single relaxed
// atomic add is always correct (incl. once worker threads exist) and far cheaper
// than the live-set's hash-table mutex, so it needs no mt fast-path.
static int64_t __cajeta_total_allocated = 0;

static inline void __cajeta_note_alloc(uint64_t bytes) {
    __atomic_fetch_add(&__cajeta_total_allocated, (int64_t) bytes, __ATOMIC_RELAXED);
}

int64_t __cajeta_total_allocated_bytes(void) {
    return __atomic_load_n(&__cajeta_total_allocated, __ATOMIC_RELAXED);
}

static void __cajeta_live_set_add_locked(void* p) {
    if (!p || p == CAJETA_LIVE_SET_TOMBSTONE) return;
    if (__cajeta_live_set_count >= CAJETA_LIVE_SET_LOAD_CAP) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                "cajeta: live-allocation set reached load cap (%d / %d). "
                "Subsequent allocations won't be tracked; field auto-drop "
                "may double-free aliased addresses past this point. "
                "Raise CAJETA_LIVE_SET_CAPACITY in runtime/native/cajeta_runtime.c "
                "(see docs/FieldOwnership.md).\n",
                __cajeta_live_set_count, CAJETA_LIVE_SET_CAPACITY);
            warned = 1;
        }
        return;
    }
    size_t mask = CAJETA_LIVE_SET_CAPACITY - 1;
    size_t bucket = ((uintptr_t) p >> 3) & mask;
    for (size_t i = 0; i < CAJETA_LIVE_SET_CAPACITY; i++) {
        size_t idx = (bucket + i) & mask;
        void* cur = __cajeta_live_set[idx];
        if (cur == NULL || cur == CAJETA_LIVE_SET_TOMBSTONE) {
            __cajeta_live_set[idx] = p;
            __cajeta_live_set_count++;
            return;
        }
        if (cur == p) return;  // already present (shouldn't happen — alloc gave a fresh address)
    }
}

static int __cajeta_live_set_remove_locked(void* p) {
    if (!p || p == CAJETA_LIVE_SET_TOMBSTONE) return 0;
    size_t mask = CAJETA_LIVE_SET_CAPACITY - 1;
    size_t bucket = ((uintptr_t) p >> 3) & mask;
    for (size_t i = 0; i < CAJETA_LIVE_SET_CAPACITY; i++) {
        size_t idx = (bucket + i) & mask;
        void* cur = __cajeta_live_set[idx];
        if (cur == NULL) return 0;  // empty slot — search ends
        if (cur == p) {
            __cajeta_live_set[idx] = CAJETA_LIVE_SET_TOMBSTONE;
            __cajeta_live_set_count--;
            return 1;
        }
    }
    return 0;
}

// Single-threaded fast path. The live-set mutex only guards against a SECOND
// thread; until one exists every alloc/drop is on the main thread, so the
// lock/unlock is pure overhead (a hot cost — alloc-heavy code does millions).
// This flag is one-way 0->1, flipped on the main thread (release barrier) BEFORE
// any worker carrier / timer / reactor / kernel-pool thread is pthread_create'd
// (see __cajeta_live_set_go_multithreaded). While 0, the only thread that can
// touch the set is the one reading the flag, so the table op is safe lock-free;
// once 1, every caller — including the just-spawned thread, which sees 1 through
// the spawn's barrier — takes the mutex.
static volatile int __cajeta_live_set_mt = 0;

void __cajeta_live_set_go_multithreaded(void) {
    __atomic_store_n(&__cajeta_live_set_mt, 1, __ATOMIC_RELEASE);
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

// Returns 1 if the address was in the set and has been removed; 0 otherwise.
// This is the atomic "claim" used by drop dispatchers: only the first caller
// gets a 1 and is responsible for running the destructor + free.
int __cajeta_live_set_claim(void* p) {
    if (__atomic_load_n(&__cajeta_live_set_mt, __ATOMIC_ACQUIRE) == 0) {
        return __cajeta_live_set_remove_locked(p);
    }
    pthread_mutex_lock(&__cajeta_live_set_mu);
    int r = __cajeta_live_set_remove_locked(p);
    pthread_mutex_unlock(&__cajeta_live_set_mu);
    return r;
}

// Per-thread "ownership transfer" mask for the caller-side `#x` -> plain-`T`
// param protocol (OwnershipTransfer.md). At a call site with one or more `#`
// arguments the compiler stores a bitmask here (bit i set iff user-arg i was
// transferred) immediately before the call, and clears it to 0 immediately
// after. The callee reads it once at entry via Cajeta.moveMask() to learn which
// of its parameters it OWNS (and must drop) vs merely borrows. Thread-local so
// concurrent callers don't clobber each other; 0 between calls, so a call with
// no `#` args correctly reads 0.

// Per-thread paired RETURN flag (title-tracking spec §4.2/§4.4): a
// class-pointer-returning method stores 1 (title travels to the caller) or 0
// (borrow) immediately before its `ret` — after all scope-exit drops — and
// the caller reads it immediately after the call instruction. Nothing can
// execute between those two points (no call, no fiber suspension), so unlike
// the deprecated arg-side mask there is no forwarding chain to lose it.
static __thread int64_t __cajeta_return_flag_tls = 0;
int64_t __cajeta_return_flag_get(void) { return __cajeta_return_flag_tls; }
void __cajeta_return_flag_set(int64_t f) { __cajeta_return_flag_tls = f; }

// Allocate and zero-fill a buffer holding total_count elements of elem_size bytes.
// Used for primitive-element arrays.
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

// Allocate a Java-style array header — { i64 size, [count x elem] data } — laid
// out as one contiguous heap block. Stores `count` into the size field at offset 0
// and zero-fills the data region. The compiler emits one call per array level for
// multi-dim shapes (outer first, then per-element inner allocations).
//
// header_size is the offset of the data region (typically 8 for an i64 size field
// with no padding, but the compiler queries DataLayout::getTypeAllocSize on the
// header struct to be safe under alignment).
void* __cajeta_new_array_header(uint64_t header_size, uint64_t elem_size, uint64_t count) {
    // Overflow guard: total = header_size + count*elem_size in uint64 would wrap
    // (e.g. `new int[-1]` arrives as count=0xFFFF...), and `calloc(1, total)`
    // performs no nmemb*size check, so a wrapped `total` under-allocates and the
    // count store + element writes overrun the heap. Reject before computing.
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
    // Store count at the size field (first 8 bytes of the header).
    *((int64_t*) hdr) = (int64_t) count;
    __cajeta_note_alloc(total);
    __cajeta_live_set_add(hdr);
    return hdr;
}

// title-stores §3.1 — droppable-element arrays carry a per-slot ownership
// bitmap in a TAIL after the data region: { i64 count | data[count*elem] |
// bits[ceil(count/8)] }, one contiguous zeroed block. The tail address is
// computed from the count word (header layout unchanged, no extra pointer).
// Unit 2: allocation + accounting only; the bits are written by the Unit-3
// store/teardown codegen.
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

// Same as __cajeta_new_array_header but the data region is left UNINITIALIZED
// (malloc, not calloc). For buffers the caller fully overwrites before reading
// — e.g. StringBuilder grow/toString, String concat payloads — the calloc
// zero-fill is pure waste (O(bytes) stores immediately clobbered). The count
// header is still set and the block is still live-set tracked, so drop / free
// behave identically to a zeroed array; only the zeroing is skipped.
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

// Materialize a cajeta `String[]` from C `argv` for a `main(String[] args)`
// entry point. The String struct's total size and field byte offsets, plus the
// String vtable, are passed in from the emit shim (computed via LLVM's
// DataLayout on the real class type) so nothing about the String ABI is
// hardcoded here. Returns a standard CajetaArray `{ i64 count, [count x ptr] }`
// of owned (mode=0) String instances, each holding a heap copy of an argv slot.
void* __cajeta_args_make(int64_t argc, char** argv,
                         void* string_vtable, int64_t str_size,
                         int64_t off_lentag, int64_t off_aux,
                         int64_t off_base, int64_t off_cplen) {
    if (argc < 0) argc = 0;
    // cajeta `String[]` has array LLVM type `{ i64, [0 x %String] }`, so the
    // element STRIDE is the full String struct size — but each slot holds a
    // `String*` POINTER in its first 8 bytes (the codegen stores/loads a
    // pointer per element; see the aggregate-init lowering). So: allocate the
    // backing with `str_size` stride, then store one heap String* per slot.
    // 6.2.2 tagged core: the offsets are (lenTag, aux, base, cachedCpLength);
    // aux+base are contiguous, so Inline text writes span both.
    void* arr = __cajeta_new_array_header(8, (uint64_t) str_size, (uint64_t) argc);
    char* base = (char*) arr + 8;
    for (int64_t i = 0; i < argc; i++) {
        const char* s = (argv && argv[i]) ? argv[i] : "";
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
        // Store the pointer at the (str_size-strided) element slot.
        *(void**) (base + (size_t) i * (size_t) str_size) = str;
    }
    return arr;
}

// Idempotent — see FieldOwnership.md § Solution B. Auto field drop and the
// owning local's chain pop both call this for the same array address; the
// first one wins the live-set claim and actually frees, the second sees
// the address is gone and returns silently.
int __cajeta_shared_owner_drop(void* base);   // cajeta_rt_shared.c (same TU)

void __cajeta_free_array(void* ptr) {
    if (!ptr) return;
    // Shared-state seam (slice-spec §3.6): sign bit clear -> plain claim as
    // today; set -> the owner's stake releases, free only at the last stake.
    if (!__cajeta_shared_owner_drop(ptr)) return;
    __cajeta_poison_buffer(ptr);
    free(ptr);
}

// element-ownership §7.1.4 — drop the live elements of an OWNING container's
// backing array, ahead of __cajeta_free_array on the buffer itself. `count`
// is the container's @ElementCount-designated live count, NOT the header
// word (which stores allocated capacity — slots past the live count are
// uninitialized and must not be touched). `header` and `stride` mirror the
// __cajeta_new_array_header allocation exactly (both DataLayout sizes
// computed by the compiler): String elements occupy inline 64-byte value
// slots, class refs 8-byte ptr slots — in both layouts the reference
// pointer sits at the SLOT BASE (the element-store codegen writes it
// there). `drop_fn` is compiler-chosen per the monomorphized element type
// (__cajeta_string_drop / __cajeta_class_virtual_drop), both idempotent via
// the live-set claim, so an element also owned elsewhere frees exactly once.
void __cajeta_drop_array_elements(void* arr, int64_t count, int64_t header,
                                  int64_t stride, void (*drop_fn)(void*)) {
    if (arr == NULL || drop_fn == NULL || stride <= 0) return;
    char* data = (char*) arr + header;
    for (int64_t i = 0; i < count; i++) {
        void* elem = *(void**) (data + i * stride);
        if (elem != NULL) drop_fn(elem);
    }
}

// Drop helper for OWNING views (Views.md § Construction).
// A view value is a pointer into the data region of a byte[] (the view-ctor
// codegen GEPs past the 8-byte array header). For an owning view, scope-exit
// must free the underlying array header, not the data pointer. This helper
// recovers the header by subtracting the header offset and then calls
// __cajeta_free_array. Kept as a dedicated symbol so the drop-fn pointer
// types match (void(*)(void*)) without callers needing to know the offset.
void __cajeta_view_drop_owned(void* data_ptr) {
    if (data_ptr == NULL) return;
    void* header = (void*) ((char*) data_ptr - 8);
    if (!__cajeta_shared_owner_drop(header)) return;
    __cajeta_poison_buffer(header);
    free(header);
}

// Materialize a heap-allocated T[] from a view's variable-size T[] field
// (S5b — Views.md § Variable-size fields). The view's bytes don't include
// an array header — they're just `count * elem_size` packed element bytes.
// This helper allocates a fresh array header + data block and memcpys the
// element bytes into it. Returns the header pointer (matches the layout
// __cajeta_new_array_header produces).
//
// Caller responsibility: free the returned pointer via the standard array
// drop path when the result goes out of scope. The drop entry for the
// caller's local is registered by LocalVariableDeclaration as usual.
void* __cajeta_array_view_to_owned(const void* data, int64_t count, int64_t elem_size) {
    if (count < 0) count = 0;
    if (elem_size <= 0) elem_size = 1;
    uint64_t header_size = 8;
    // Same overflow guard as __cajeta_new_array_header (count/elem are clamped
    // non-negative above but the product can still wrap uint64).
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

// Generic zero-fill allocation for compiler-emitted heap blocks that don't
// match an array shape — used for closure records and captures structs in
// L3-3. Mirrors __cajeta_new_array's failure mode so the compiler doesn't
// have to handle null returns.
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

// Uninitialized counterpart of __cajeta_alloc (malloc, not calloc) for blocks
// the emitter fully overwrites before any read — e.g. the String-concat byte
// buffer, which stores its count word and memcpys data+NUL across the whole
// block. Still live-set tracked, so __cajeta_free reclaims it identically.
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

// Mirror of __cajeta_free_array for non-array heap blocks. Kept as a
// separate symbol so the drop-fn function-pointer types match what the
// emitted IR uses for arrays (both are `void(*)(void*)`).
//
// Unconditional: this is the actual body-free that runs at the end of
// every class drop wrapper, after __cajeta_class_virtual_drop has
// already claimed the instance out of the live-set. Idempotency for
// class instances lives in virtual_drop's claim, not here.
void __cajeta_free(void* ptr) {
    if (!ptr) return;
    __cajeta_live_set_claim(ptr);  // remove if present; ignore result
    __cajeta_poison_buffer(ptr);
    free(ptr);
}

// ---- Frame bump arena (docs/specs/frame-arena-spec.md) ---------------------
// Thread-local bump allocator backing non-escaping owned locals (e.g. transient
// String concat temporaries). Allocate = bump a pointer; reclaim = O(1) reset to a
// saved mark. Arena blocks are NOT live-set tracked and never individually freed —
// a whole scope's worth is reclaimed at scope exit by one reset.
//
// One large virtual reservation per thread, committed lazily by the kernel
// (MAP_NORESERVE: pages fault in zero-filled on first touch, so they never move and
// existing pointers stay valid as the bump advances). A mark is just the current
// byte offset, so mark/reset are a load/store and nest LIFO with lexical scopes.
// Reset optionally madvise(DONTNEED)s pages above the new mark once retained
// capacity exceeds a threshold — the memory-pressure backstop (spec 6.1.5); pages
// come back zero-filled on the next touch.
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
    size_t count;              // live arena-array allocations — restores the
                               // pre-frame-arena dropCount tick (one per array
                               // reclaimed) on the O(1) reset; see __cajeta_arena_reset
    size_t committed;          // Windows: bytes VirtualAlloc(MEM_COMMIT)'d so far;
                               // POSIX: set to RESERVE (the kernel commits on fault)
} cajeta_arena;

// The NON-FIBER arena: main thread + any native helper thread. Carrier-hosted
// fibers each own a `cajeta_arena` inside their `struct cajeta_fiber` instead —
// the LIFO mark/reset discipline the arena depends on holds per LOGICAL stack,
// and a carrier interleaves many fiber stacks. With a single per-thread arena a
// fiber that PARKED mid-frame (await, readAsync, …) left its live allocations
// above marks that co-hosted fibers would reset right through: the resumed
// fiber then read/wrote recycled bytes (http:0.11 — the client-loopback
// index-0-of-empty SIGABRT; kin to the double-close and lambda-capture
// double-free reports). Same per-fiber rationale — and the same accessor
// pattern — as scope_top/drop_top/exc_top/fl_top.
static __thread cajeta_arena __cajeta_arena = { NULL, 0, 0, 0, 0 };

// Selects the current context's arena: the running fiber's own, else the
// thread's. Defined in cajeta_rt_concurrent_exec.c (same TU — see
// cajeta_runtime.c's include order) where __cajeta_current_fiber lives.
static cajeta_arena* __cajeta_arena_ptr(void);

// --- arena-mapping pool ------------------------------------------------------
// Fibers are born and die constantly; mmap/munmap per fiber would churn. A
// completed fiber's mapping goes on this free-list and the next arena init
// (any thread, any fiber) reuses it. Entries are tiny malloc'd nodes; the
// mappings themselves are uniform CAJETA_ARENA_RESERVE reservations. Depth is
// bounded by peak concurrent arena-USING contexts (lazy init means fibers that
// never arena-allocate never hold a mapping). Process-lifetime, like the
// per-thread arenas — never unmapped at shutdown.
struct cajeta_arena_pooled {
    unsigned char* base;
    size_t retained;
    size_t committed;
    struct cajeta_arena_pooled* next;
};
static struct cajeta_arena_pooled* __cajeta_arena_pool = NULL;
static pthread_mutex_t __cajeta_arena_pool_mu = PTHREAD_MUTEX_INITIALIZER;

// Defined in cajeta_rt_concurrent_sync.c (included later in this TU). Adds a batch
// of drops to the test drop-count — used by the arena reset to account for the
// non-escaping primitive arrays it reclaims in bulk.
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
    // No MAP_NORESERVE on Windows: reserve the address range now, commit pages
    // lazily as the bump advances (__cajeta_arena_bump). Reserved-but-uncommitted
    // pages fault on access, so a bump must commit before it hands out the region.
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

// Return a dead fiber's mapping to the pool (no-op if it never allocated).
// Called from the fiber teardown paths in cajeta_rt_concurrent_exec.c. When
// the fiber retained real memory, hand the pages back to the OS first so a
// deep-recursion outlier doesn't pin RSS from inside the pool.
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

// __attribute__((malloc)): each bump returns a fresh, disjoint region, so the
// result aliases no other live pointer — exactly malloc's contract (the regions
// are reused only AFTER a scope-exit reset, past every use of the old pointer).
// WITHOUT this, LLVM cannot prove two arena arrays (e.g. fannkuch's perm/perm1/
// count, all offsets off the same arena base) don't alias, so it reloads array
// bases across every store in hot loops — a ~2.3x regression on integer-array
// code when frame-arena U3 began routing primitive arrays here. malloc restores
// the noalias the malloc path gets for free, and propagates through inlining.
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

// Zeroed arena alloc. Reset reuses memory, so unlike a fresh mmap page the bytes
// may be dirty — zero them for the zero-init contract the heap __cajeta_alloc has.
__attribute__((malloc)) void* __cajeta_arena_alloc(uint64_t size) {
    void* p = __cajeta_arena_bump(size);
    memset(p, 0, __cajeta_arena_align8((size_t) size));
    return p;
}

// Uninitialized arena alloc (caller overwrites every byte). Mirrors
// __cajeta_alloc_uninit.
__attribute__((malloc)) void* __cajeta_arena_alloc_uninit(uint64_t size) {
    return __cajeta_arena_bump(size);
}

// Arena variant of __cajeta_new_array_header (frame-arena-plan U3): a non-escaping
// owned heap array of primitive elements bump-allocated from the frame arena. Same
// byte layout as the malloc version (count word at offset 0, then data) so all
// array readers work unchanged, but NOT live-set tracked and never individually
// freed — the scope-exit arena reset reclaims it. Zeroed for the array zero-init
// contract (the arena reuses memory across resets, so it may be dirty).
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

// Frame-arena membership probe (slice-spec §4 arena row). A pointer inside
// the calling thread's arena reservation is frame-transient: it is recycled
// by the scope-exit reset, so it can never back a Shared stake — escaping
// slices of arena-backed buffers must COPY. Checks the full reservation
// (not just the live bump) so stale pointers into reset regions also answer
// true (they're equally unshareable).
int __cajeta_arena_owns(const void* p) {
    cajeta_arena* a = __cajeta_arena_ptr();
    return a->base
        && (const unsigned char*) p >= a->base
        && (const unsigned char*) p < a->base + CAJETA_ARENA_RESERVE;
}

// Capture the current bump offset AND the live array count. Stash at scope entry;
// pass to reset on exit. Packed: count in the high bits, bump in the low — the
// arena reserve is 4 GiB so bump < 2^32, well under bit 40, leaving 24 bits for a
// live-array count (millions). The token is opaque to the compiler (Block.cpp only
// stashes it and hands it back to reset), so packing changes nothing for codegen.
uint64_t __cajeta_arena_mark(void) {
    cajeta_arena* a = __cajeta_arena_ptr();
    return ((uint64_t) a->count << 40) | (uint64_t) a->bump;
}

// O(1) reclaim: restore the bump to `mark`. All objects allocated since the mark
// are abandoned in place (their bytes reused on the next bump). Trim backstop:
// when retained pages run well past the new mark, hand them back to the OS.
void __cajeta_arena_reset(uint64_t mark) {
    cajeta_arena* a = __cajeta_arena_ptr();
    size_t m       = (size_t) (mark & (((uint64_t) 1 << 40) - 1));
    size_t count_m = (size_t) (mark >> 40);
    // Each non-escaping primitive array reclaimed here used to free() at scope exit
    // (pre-frame-arena), ticking __cajeta_drop_count once via the drop chain. Frame-
    // arena reclaims them in one O(1) bump-reset — no per-array free, no live-set —
    // so account for the delta here in bulk. Keeps drop-count probes accurate
    // without re-adding any per-array cost. Test-only instrumentation.
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

// cajeta.lang.String wrapper layout (generatePrototype embed order, mirrored
// by the literal/concat lowerings) — slices plan 6.2.2: the storage is the
// 16-byte tagged Utf8 core (slice-spec §8), `mode` collapsed into the tag.
//   len <= 12            Inline — text lives in `data`; self-contained.
//   len >  12            pointer form: data overlays {i32 off, char* base};
//                        base is always a ROOT CajetaArray header, text at
//                        base + 8 + off. Tag bits:
//     CAJ_STR_SHARED_BIT  wrapper holds one rc stake on base (drop releases,
//                         copies retain — the count-word sign convention).
//     CAJ_STR_BORROW_BIT  stakeless view of a heap root (slices plan 4.2.2;
//                         drop must not touch the root).
//     CAJ_STR_STATIC_BIT  static root (literals + views of them); no rc ever.
//     no bits             OWNED sole root — drop frees via the owner-drop
//                         seam; never enters the shared table unless a slice
//                         escapes (zero rc traffic for never-shared strings).
// Pointer forms always carry len > 12 (the §8 normalization rule: every
// <= 12 B result is built Inline), so the discrimination is total.
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
// Build a fresh owned root (count word + text + NUL) holding src[0..len);
// returns the header. Callers wrap it as {len, 0, buf} with no tag bits.
static inline void* caj_str_new_root(const char* src, int32_t len) {
    void* buf = __cajeta_new_array_header(8, 1, (uint64_t) len + 1);
    *((int64_t*) buf) = len;
    if (len > 0) memcpy((char*) buf + 8, src, (size_t) len);
    ((char*) buf)[8 + len] = 0;
    return buf;
}

// Mode-aware drop for cajeta.lang.String locals/owned values
// (docs/specification/lang/String.md § Memory model — the owned/view distinction
// the drop chain was always designed for). Frees the byte buffer ONLY for owned
// strings (mode 0); view strings borrow their bytes and must never free them.
// The live-set claim makes this idempotent and a no-op on static literal
// wrappers (which aren't tracked). Drop-fn shape: void(*)(void*).
// Claim-assumed variant: the CALLER already won this wrapper's live-set
// claim (__cajeta_class_virtual_drop claims before dispatching the vtable
// drop_fn — FieldOwnership.md § Solution B). Runs the tag-dispatched root
// work and frees the wrapper. Never call without holding the claim.
void __cajeta_string_drop_claimed(void* s) {
    cajeta_string_layout* str = (cajeta_string_layout*) s;
    int32_t tag = str->lenTag;
    char* base = caj_str_is_pointer(str) ? caj_str_base(str) : NULL;
    // Tag dispatch (slice-spec §8.2). Inline is self-contained; BORROW views
    // hold no stake; STATIC roots are never freed. SHARED releases this
    // wrapper's stake (the last stake frees the root). OWNED (no bits)
    // routes through the array owner-drop seam: claim-and-free the sole
    // root, or release the owner's stake if a slice escape promoted it.
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

void __cajeta_string_drop(void* s) {
    if (!s) return;
    if (!__cajeta_live_set_claim(s)) return;   // static wrapper / already freed
    __cajeta_string_drop_claimed(s);
}

// Drop dispatcher for function-typed locals. The drop chain registers
// this generic helper for every function-typed local; at scope exit it
// reads the closure record's drop_fn slot and invokes it if non-null.
// Non-capturing closures (whose record is a global constant with
// drop_fn=null) and null pointers are no-ops, so the same drop-entry
// shape is safe for every assignment.
//
// Closure record layout (L3-3): { void* fn, void* captures, void(*drop_fn)(void*) }
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

// --- Threading sync primitives: Lock --------------------------------------
