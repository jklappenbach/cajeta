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
};

// Selector for the current dbg frame chain head — mirrors __cajeta_scope_top_ptr
// (fiber vs main TLS). Defined further down where __cajeta_current_fiber is in
// scope; forward-declared here so the enter/leave/local helpers can use it.
struct cajeta_dbg_frame** __cajeta_dbg_top_ptr(void);

void __cajeta_dbg_frame_enter(const char* func) {
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
    *top = f;
}

void __cajeta_dbg_frame_leave(void) {
    struct cajeta_dbg_frame** top = __cajeta_dbg_top_ptr();
    struct cajeta_dbg_frame* f = *top;
    if (!f) return;
    *top = f->prev;
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
// leaves no stale entries. Fiber line-info is deferred (spec §1.5): a fiber's
// enter/mark/leave run on the carrier thread's TLS stack, which can leave stale
// entries across a yield — bounded + memory-safe (fixed array), never resolved
// for in-fiber throws (trace capture already skips fibers).
typedef struct {
    const char* typeName;    // "test.App"
    const char* methodName;  // "run"
    const char* fileName;    // "App.cajeta"
} CajetaFrameDesc;

typedef struct {
    const CajetaFrameDesc* desc;
    int32_t line;
} CajetaShadowFrame;

#define CAJETA_SHADOW_MAX 512
static __thread CajetaShadowFrame __cajeta_shadow[CAJETA_SHADOW_MAX];
static __thread int32_t __cajeta_shadow_top = 0;

void __cajeta_line_enter(const void* desc) {
    int32_t t = __cajeta_shadow_top;
    if (t >= 0 && t < CAJETA_SHADOW_MAX) {
        __cajeta_shadow[t].desc = (const CajetaFrameDesc*) desc;
        __cajeta_shadow[t].line = 0;
    }
    __cajeta_shadow_top = t + 1;   // count past the cap so leave stays balanced
}
void __cajeta_line_mark(int32_t line) {
    int32_t t = __cajeta_shadow_top;
    if (t > 0 && t <= CAJETA_SHADOW_MAX) __cajeta_shadow[t - 1].line = line;
}
void __cajeta_line_leave(void) {
    if (__cajeta_shadow_top > 0) __cajeta_shadow_top--;
}
int32_t __cajeta_shadow_get_top(void) { return __cajeta_shadow_top; }
void __cajeta_shadow_set_top(int32_t watermark) {
    if (watermark >= 0) __cajeta_shadow_top = watermark;
}
// Snapshot the live shadow frames innermost-first into `out` (caller-sized to
// `max`), returning the number copied. `out[0]` is the throw-site frame.
int32_t __cajeta_shadow_snapshot(CajetaShadowFrame* out, int32_t max) {
    int32_t n = __cajeta_shadow_top;
    if (n > CAJETA_SHADOW_MAX) n = CAJETA_SHADOW_MAX;  // deepest-past-cap unstored
    int32_t w = 0;
    for (int32_t i = n - 1; i >= 0 && w < max; i--) out[w++] = __cajeta_shadow[i];
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
    int32_t n = __cajeta_shadow_top;
    if (n > CAJETA_SHADOW_MAX) n = CAJETA_SHADOW_MAX;
    if (n <= 0) {
        fprintf(out, "  <no cajeta frames: line-info off, or not in cajeta code>\n");
        fflush(out);
        return;
    }
    for (int32_t i = n - 1; i >= 0; i--) {
        const CajetaFrameDesc* d = __cajeta_shadow[i].desc;
        const char* t = (d && d->typeName)   ? d->typeName   : "?";
        const char* m = (d && d->methodName) ? d->methodName : "?";
        const char* f = (d && d->fileName)   ? d->fileName   : "?";
        // Basename only, matching the captured-trace format.
        const char* base = f;
        for (const char* q = f; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
        fprintf(out, "  at %s.%s(%s:%d)\n", t, m, base, __cajeta_shadow[i].line);
    }
    fflush(out);
}

// Depth of the live shadow stack, and one frame by index (0 = innermost).
// Field accessors rather than a struct return, so a debugger — or any consumer
// that cannot see this file's types without debug info — can walk frames
// through plain calls.
__attribute__((used, retain))
int32_t __cajeta_stack_depth(void) {
    int32_t n = __cajeta_shadow_top;
    return n > CAJETA_SHADOW_MAX ? CAJETA_SHADOW_MAX : (n < 0 ? 0 : n);
}
__attribute__((used, retain))
const char* __cajeta_stack_type(int32_t i) {
    int32_t n = __cajeta_stack_depth();
    if (i < 0 || i >= n) return "";
    const CajetaFrameDesc* d = __cajeta_shadow[n - 1 - i].desc;
    return (d && d->typeName) ? d->typeName : "?";
}
__attribute__((used, retain))
const char* __cajeta_stack_method(int32_t i) {
    int32_t n = __cajeta_stack_depth();
    if (i < 0 || i >= n) return "";
    const CajetaFrameDesc* d = __cajeta_shadow[n - 1 - i].desc;
    return (d && d->methodName) ? d->methodName : "?";
}
__attribute__((used, retain))
const char* __cajeta_stack_file(int32_t i) {
    int32_t n = __cajeta_stack_depth();
    if (i < 0 || i >= n) return "";
    const CajetaFrameDesc* d = __cajeta_shadow[n - 1 - i].desc;
    return (d && d->fileName) ? d->fileName : "?";
}
__attribute__((used, retain))
int32_t __cajeta_stack_line(int32_t i) {
    int32_t n = __cajeta_stack_depth();
    if (i < 0 || i >= n) return 0;
    return __cajeta_shadow[n - 1 - i].line;
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
int __cajeta_dbg_frame_depth(void* top) {
    int n = 0;
    for (struct cajeta_dbg_frame* f = top; f; f = f->prev) n++;
    return n;
}
void* __cajeta_dbg_frame_prev(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->prev : NULL;
}
const char* __cajeta_dbg_frame_func(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->func : NULL;
}
int32_t __cajeta_dbg_frame_loc(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->current_loc : -1;
}
int __cajeta_dbg_frame_nlocals(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->nlocals : 0;
}
const char* __cajeta_dbg_local_name(void* frame, int i) {
    if (!frame) return NULL;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return NULL;
    return f->locals[i].name;
}
const char* __cajeta_dbg_local_type(void* frame, int i) {
    if (!frame) return NULL;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return NULL;
    return f->locals[i].type;
}
void* __cajeta_dbg_local_addr(void* frame, int i) {
    if (!frame) return NULL;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return NULL;
    return f->locals[i].addr;
}
// CP7-1b: the two memory facets. Out-of-range reads back 0 (== Unknown), the
// same neutral fallback codegen uses when a facet isn't statically known.
uint8_t __cajeta_dbg_local_alloc(void* frame, int i) {
    if (!frame) return 0;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return 0;
    return f->locals[i].alloc;
}
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

void __cajeta_dbg_safepoint(int32_t loc_id) {
    __cajeta_dbg_safepoint_total++;
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

void __cajeta_dbg_reset_safepoint_count(void) {
    __cajeta_dbg_safepoint_total = 0;
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
static __thread int64_t __cajeta_move_mask_tls = 0;
int64_t __cajeta_move_mask_get(void) { return __cajeta_move_mask_tls; }
void __cajeta_move_mask_set(int64_t m) { __cajeta_move_mask_tls = m; }

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

static __thread cajeta_arena __cajeta_arena = { NULL, 0, 0, 0, 0 };

// Defined in cajeta_rt_concurrent_sync.c (included later in this TU). Adds a batch
// of drops to the test drop-count — used by the arena reset to account for the
// non-escaping primitive arrays it reclaims in bulk.
void __cajeta_drop_count_add(int64_t n);

static void __cajeta_arena_init(void) {
#if defined(_WIN32)
    // No MAP_NORESERVE on Windows: reserve the address range now, commit pages
    // lazily as the bump advances (__cajeta_arena_bump). Reserved-but-uncommitted
    // pages fault on access, so a bump must commit before it hands out the region.
    void* p = VirtualAlloc(NULL, CAJETA_ARENA_RESERVE, MEM_RESERVE, PAGE_READWRITE);
    if (!p) {
        fprintf(stderr, "cajeta: arena VirtualAlloc(MEM_RESERVE) failed\n");
        abort();
    }
    __cajeta_arena.committed = 0;
#else
    void* p = mmap(NULL, CAJETA_ARENA_RESERVE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "cajeta: arena mmap failed\n");
        abort();
    }
    __cajeta_arena.committed = CAJETA_ARENA_RESERVE;   // kernel commits on first touch
#endif
    __cajeta_arena.base = (unsigned char*) p;
    __cajeta_arena.bump = 0;
    __cajeta_arena.retained = 0;
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
    if (!__cajeta_arena.base) __cajeta_arena_init();
    size_t n = __cajeta_arena_align8((size_t) size);
    if (__cajeta_arena.bump + n > CAJETA_ARENA_RESERVE) {
        fprintf(stderr, "cajeta: arena exhausted (request=%zu, reserve=%zu)\n",
                (size_t) size, (size_t) CAJETA_ARENA_RESERVE);
        abort();
    }
    unsigned char* p = __cajeta_arena.base + __cajeta_arena.bump;
    __cajeta_arena.bump += n;
    if (__cajeta_arena.bump > __cajeta_arena.retained) {
        __cajeta_arena.retained = __cajeta_arena.bump;
    }
#if defined(_WIN32)
    // Commit the page(s) the new [p, p+n) region spans before returning it.
    if (__cajeta_arena.bump > __cajeta_arena.committed) {
        size_t pg = 4096;                                   // x64 Windows page
        size_t need = (__cajeta_arena.bump + (pg - 1)) & ~(pg - 1);
        if (need > CAJETA_ARENA_RESERVE) need = CAJETA_ARENA_RESERVE;
        if (!VirtualAlloc(__cajeta_arena.base + __cajeta_arena.committed,
                          need - __cajeta_arena.committed,
                          MEM_COMMIT, PAGE_READWRITE)) {
            fprintf(stderr, "cajeta: arena VirtualAlloc(MEM_COMMIT) failed\n");
            abort();
        }
        __cajeta_arena.committed = need;
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
    __cajeta_arena.count++;   // reclaimed (and drop-counted) at the next scope reset
    return hdr;
}

// Frame-arena membership probe (slice-spec §4 arena row). A pointer inside
// the calling thread's arena reservation is frame-transient: it is recycled
// by the scope-exit reset, so it can never back a Shared stake — escaping
// slices of arena-backed buffers must COPY. Checks the full reservation
// (not just the live bump) so stale pointers into reset regions also answer
// true (they're equally unshareable).
int __cajeta_arena_owns(const void* p) {
    return __cajeta_arena.base
        && (const unsigned char*) p >= __cajeta_arena.base
        && (const unsigned char*) p < __cajeta_arena.base + CAJETA_ARENA_RESERVE;
}

// Capture the current bump offset AND the live array count. Stash at scope entry;
// pass to reset on exit. Packed: count in the high bits, bump in the low — the
// arena reserve is 4 GiB so bump < 2^32, well under bit 40, leaving 24 bits for a
// live-array count (millions). The token is opaque to the compiler (Block.cpp only
// stashes it and hands it back to reset), so packing changes nothing for codegen.
uint64_t __cajeta_arena_mark(void) {
    return ((uint64_t) __cajeta_arena.count << 40) | (uint64_t) __cajeta_arena.bump;
}

// O(1) reclaim: restore the bump to `mark`. All objects allocated since the mark
// are abandoned in place (their bytes reused on the next bump). Trim backstop:
// when retained pages run well past the new mark, hand them back to the OS.
void __cajeta_arena_reset(uint64_t mark) {
    size_t m       = (size_t) (mark & (((uint64_t) 1 << 40) - 1));
    size_t count_m = (size_t) (mark >> 40);
    // Each non-escaping primitive array reclaimed here used to free() at scope exit
    // (pre-frame-arena), ticking __cajeta_drop_count once via the drop chain. Frame-
    // arena reclaims them in one O(1) bump-reset — no per-array free, no live-set —
    // so account for the delta here in bulk. Keeps drop-count probes accurate
    // without re-adding any per-array cost. Test-only instrumentation.
    if (__cajeta_arena.count > count_m) {
        __cajeta_drop_count_add((int64_t) (__cajeta_arena.count - count_m));
    }
    __cajeta_arena.count = count_m;
    __cajeta_arena.bump = m;
    if (__cajeta_arena.base
            && __cajeta_arena.retained > m + CAJETA_ARENA_TRIM_THRESHOLD) {
#if defined(_WIN32)
        size_t pg = 4096;
#else
        size_t pg = (size_t) sysconf(_SC_PAGESIZE);
        if (pg == 0) pg = 4096;
#endif
        size_t from = (m + pg - 1) & ~(pg - 1);                       // round up
        size_t to   = (__cajeta_arena.retained + pg - 1) & ~(pg - 1);
        if (to > from) {
#if defined(_WIN32)
            // Hand the pages back to the OS; a later bump re-commits them.
            VirtualFree(__cajeta_arena.base + from, to - from, MEM_DECOMMIT);
            if (__cajeta_arena.committed > from) __cajeta_arena.committed = from;
#else
            madvise(__cajeta_arena.base + from, to - from, MADV_DONTNEED);
#endif
        }
        __cajeta_arena.retained = m;
    }
}

// Test/introspection: current bytes in use, and high-water retained bytes.
int64_t __cajeta_arena_bytes(void)    { return (int64_t) __cajeta_arena.bump; }
int64_t __cajeta_arena_retained(void) { return (int64_t) __cajeta_arena.retained; }

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
