// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- Threading sync primitives: Lock --------------------------------------
//
// Cajeta's `Lock` is the no-data RAII gate from ThreadModel.md. The OS-level
// implementation is just a pthread_mutex_t — the language-side guard
// semantics (drop-on-scope-exit) layer on top. Async-aware suspension also
// layers on top, once the executor exists; for v0 these are blocking calls
// against the OS mutex, which is enough for single-thread tests and for
// the future user-facing Lock class to wrap.
//
// The intrinsic-level API is a deliberate stepping stone: Cajeta source
// invokes `Cajeta.lockNew / lockAcquire / lockRelease / lockTryAcquire /
// lockDestroy` directly via the namespace-dispatch path in
// MethodCallExpression. Once user-defined class drop lands, a `Lock` class
// will wrap these calls with an `acquire()` that returns a `LockGuard`
// whose drop calls release.

// The lock primitives live AFTER the fiber executor (further down in
// this file) so they can use the fiber struct + carrier state directly.
// Forward declarations are intentionally avoided — re-declaring `static
// __thread` storage with separate definitions is a footgun on some
// compilers, so the file is ordered: shared infrastructure first, then
// users of that infrastructure.

// --- Threading: stackful fiber executor (R3-B) ----------------------------
//
// ThreadModel.md async runtime: each spawn produces a fiber — a userspace
// task with its own stack (allocated separately, ~64KB). A single carrier
// OS thread runs many fibers cooperatively via ucontext.h. When a fiber
// calls await on a not-yet-done Task, it parks (saves its context, returns
// to the carrier); the carrier then picks another ready fiber. When any
// task completes, all parked fibers move back to the ready queue and
// re-check their await conditions. Polling-wake is inefficient (every
// completion wakes every parker) but correct; per-task wait queues land
// in a later iteration.
//
// The main thread is NOT a fiber. Its await OS-blocks on a condvar — the
// program's entry point can sit there waiting for the top-level spawned
// task to complete. This mirrors Java 21's "platform thread" vs "virtual
// thread" distinction.
//
// Why stackful (not stackless state machines)? Faster to ship — no async-
// fn codegen transformation — and removes function coloring (any function
// can call await, not just `async` ones). The per-fiber stack cost (~64KB)
// matches Java 21's virtual-thread model and is acceptable for v1. A
// stackless rewrite remains possible later if measured cost demands it.

// ucontext.h — glibc + macOS provide swapcontext / getcontext / makecontext
// for stackful coroutine context-switching. MinGW-w64 doesn't ship a
// ucontext.h (libucontext isn't packaged for mingw-w64), so on Windows we
// roll a minimal shim over the Win32 Fibers API (CreateFiber +
// SwitchToFiber + ConvertThreadToFiber), which is the canonical Windows
// equivalent for cooperative user-mode coroutines.
//
// The shim's ucontext_t struct keeps the uc_stack { ss_sp, ss_size } /
// uc_link fields the cajeta fiber init code reads, even though Windows
// manages the fiber stack internally — the fields are ignored at runtime.
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>   // BCryptGenRandom for cajeta_fill_entropy (Guid.random).
                        // Must be at file scope (after windows.h): an in-body
                        // #include doesn't declare it, and mingw ignores the
                        // MSVC `#pragma comment(lib, ...)` — the bcrypt import
                        // lib is linked from src/CMakeLists.txt instead.

typedef struct {
    LPVOID fiber;                         // Windows fiber handle
    void (*entry)(void);                  // makecontext-supplied entry
    struct { void* ss_sp; size_t ss_size; } uc_stack;
    void* uc_link;
} ucontext_t;

static VOID CALLBACK __cajeta_w32_fiber_trampoline(LPVOID param) {
    ucontext_t* uc = (ucontext_t*) param;
    if (uc && uc->entry) uc->entry();
    // The fiber function returned. Hand control to uc_link (the carrier that
    // dispatched us), mirroring ucontext's uc_link semantics.
    //
    // It MUST be uc_link, not GetCurrentFiber(): GetCurrentFiber() returns
    // THIS fiber, and SwitchToFiber(self) is undefined — in practice the
    // callback then returns and Windows terminates the carrier thread, so the
    // carrier dies after the first task finishes and any later task never runs
    // (its awaiter deadlocks). swapcontext/uc_link is the path glibc takes when
    // a makecontext fiber falls off its end.
    ucontext_t* link = uc ? (ucontext_t*) uc->uc_link : NULL;
    if (link && link->fiber) {
        SwitchToFiber(link->fiber);
    }
}

static inline int __cajeta_w32_getcontext(ucontext_t* uc) {
    if (uc) {
        uc->fiber = NULL;
        uc->entry = NULL;
        uc->uc_stack.ss_sp = NULL;
        uc->uc_stack.ss_size = 0;
        uc->uc_link = NULL;
    }
    return 0;
}

static inline void __cajeta_w32_makecontext(ucontext_t* uc, void (*func)(void), int argc) {
    (void) argc;  // cajeta always passes 0
    uc->entry = func;
    SIZE_T stack_size = uc->uc_stack.ss_size > 0
        ? (SIZE_T) uc->uc_stack.ss_size : 64 * 1024;
    uc->fiber = CreateFiber(stack_size, __cajeta_w32_fiber_trampoline, uc);
}

static inline int __cajeta_w32_swapcontext(ucontext_t* from, ucontext_t* to) {
    // First swap on the carrier thread must promote it to a fiber.
    if (!IsThreadAFiber()) {
        LPVOID cur = ConvertThreadToFiber(NULL);
        if (from) from->fiber = cur;
    } else if (from && !from->fiber) {
        from->fiber = GetCurrentFiber();
    }
    if (to && to->fiber) SwitchToFiber(to->fiber);
    return 0;
}

#define getcontext(uc)              __cajeta_w32_getcontext(uc)
#define makecontext(uc, func, argc) __cajeta_w32_makecontext(uc, func, argc)
#define swapcontext(from, to)       __cajeta_w32_swapcontext(from, to)

#else
#include <ucontext.h>
#include <sys/mman.h>   // mmap/mprotect for guard-paged fiber stacks
#endif
#include <string.h>

// Apple deprecated the ucontext.h family in 10.6 but ships no replacement for
// user-space context switching (its guidance -- GCD / pthreads -- can't express
// a cooperative fiber scheduler). ucontext is still the portable mechanism:
// glibc and the BSDs implement it, macOS implements it (just deprecated), and
// Windows is covered by the shim above. Route the three calls through wrappers
// so the residual macOS deprecation warning is silenced in exactly one place
// rather than at every call site; on every other platform these are zero-cost
// pass-throughs (and on Windows they expand to the __cajeta_w32_* shims).
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
static inline int __cajeta_getcontext(ucontext_t* uc) {
    return getcontext(uc);
}
static inline void __cajeta_makecontext(ucontext_t* uc, void (*func)(void), int argc) {
    makecontext(uc, func, argc);
}
static inline int __cajeta_swapcontext(ucontext_t* from, ucontext_t* to) {
    return swapcontext(from, to);
}
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif

// Per-fiber stack size. A fiber runs arbitrary Cajeta code, which routinely
// calls down into native C libraries (OpenSSL during a TLS handshake parses an
// X.509 chain, runs ECDSA P-256, decodes ASN.1 — a call tree that alone wants
// well over 64 KB of stack). 64 KB was enough for the shallow channel/fan-out
// fibers the early tests exercised but far too little for a fiber that drives
// a real native library; size it like a conventional OS thread stack (1 MB) so
// any reasonable native call depth fits. Overridable via $CAJETA_FIBER_STACK_KB
// for pathological depths or tight memory budgets.
//
// On POSIX the stack is an mmap'd region with a PROT_NONE guard page below it
// (stacks grow down), so an overflow faults cleanly AT the overflow site
// instead of silently scribbling over adjacent heap and surfacing later as
// heap corruption far from the bug (see __cajeta_fiber_stack_alloc). On
// Windows the Win32 fiber shim ignores this buffer entirely — CreateFiber
// allocates its own stack with the OS's standard guard pages.
#define CAJETA_FIBER_STACK_SIZE (1024 * 1024)

// Resolve the per-fiber stack size once, honoring $CAJETA_FIBER_STACK_KB (a
// size in KiB) when set to a sane positive value, else CAJETA_FIBER_STACK_SIZE.
// Cached in a static so every fiber in the process gets the same size without
// re-parsing the environment. A clamp floors the override at 64 KiB so a
// mis-set tiny value can't reintroduce the overflow this default guards against.
static size_t __cajeta_fiber_stack_size(void) {
    static size_t cached = 0;
    if (cached == 0) {
        size_t sz = (size_t) CAJETA_FIBER_STACK_SIZE;
        const char* env = getenv("CAJETA_FIBER_STACK_KB");
        if (env && *env) {
            long kb = atol(env);
            if (kb >= 64) sz = (size_t) kb * 1024;
        }
        cached = sz;
    }
    return cached;
}

// Allocate / free one fiber stack of __cajeta_fiber_stack_size() bytes.
//
// POSIX: an anonymous mmap of guard-page + stack, with the LOWEST page made
// PROT_NONE — stacks grow down on every supported target, so an overflow hits
// the guard and faults cleanly at the overflowing frame instead of corrupting
// whatever the heap happened to place below the buffer. The returned pointer
// is the USABLE base (just above the guard); ss_sp/ss_size wiring at the
// call site is unchanged. The stack size is rounded up to a page multiple so
// free can reconstruct the exact mapping from the cached size alone.
//
// Windows: plain malloc, as before — the Win32 fiber shim never uses this
// buffer (CreateFiber allocates its own guarded stack); it exists only so the
// fiber init/teardown paths stay uniform across platforms.
//
// Allocation failure aborts with a message (matches the prior malloc-fail
// behavior): a program that cannot allocate a fiber stack cannot run its task.
static size_t __cajeta_fiber_stack_alloc_size(void) {
#if defined(_WIN32)
    return __cajeta_fiber_stack_size();
#else
    static size_t cached = 0;
    if (cached == 0) {
        size_t page = (size_t) sysconf(_SC_PAGESIZE);
        size_t sz = __cajeta_fiber_stack_size();
        cached = (sz + page - 1) & ~(page - 1);   // round up to page multiple
    }
    return cached;
#endif
}

static void* __cajeta_fiber_stack_alloc(void) {
#if defined(_WIN32)
    void* p = malloc(__cajeta_fiber_stack_alloc_size());
    if (!p) {
        fprintf(stderr, "cajeta: fiber stack malloc failed\n");
        abort();
    }
    return p;
#else
    size_t page = (size_t) sysconf(_SC_PAGESIZE);
    size_t size = __cajeta_fiber_stack_alloc_size();
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_STACK)
    flags |= MAP_STACK;   // advisory on Linux; tells the kernel it's a stack
#endif
    void* base = mmap(NULL, page + size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "cajeta: fiber stack mmap failed\n");
        abort();
    }
    if (mprotect(base, page, PROT_NONE) != 0) {
        fprintf(stderr, "cajeta: fiber stack guard mprotect failed\n");
        abort();
    }
    return (char*) base + page;
#endif
}

static void __cajeta_fiber_stack_free(void* stack) {
    if (!stack) return;
#if defined(_WIN32)
    free(stack);
#else
    size_t page = (size_t) sysconf(_SC_PAGESIZE);
    munmap((char*) stack - page, page + __cajeta_fiber_stack_alloc_size());
#endif
}

typedef void (*cajeta_task_trampoline_fn)(void* arg);

typedef enum {
    CAJETA_FIBER_READY,     // on the ready queue, will be resumed
    CAJETA_FIBER_RUNNING,   // on the carrier right now
    CAJETA_FIBER_PARKED,    // suspended at await, awaits global wake
    CAJETA_FIBER_DONE,      // trampoline returned; carrier will free
} cajeta_fiber_state;

// Forward declaration — scope_exit re-raises via __cajeta_throw, which
// is defined later in the file.
__attribute__((noreturn)) void __cajeta_throw(void* value);

// R5-A/R5-D: per-scope tracking of spawned child tasks. The scope's
// closing `}` calls __cajeta_scope_exit which waits for every registered
// task's done flag before returning, guaranteeing the doc's "control
// doesn't leave the block until every child has finished" property. R5-D
// adds the exception slot: after waiting, scope_exit walks each task's
// exception slot and re-raises the first one found.
struct cajeta_scope_entry {
    int32_t* done_addr;
    void** exception_addr;  // points to the Throwable* slot; NULL on success
    void** fiber_slot;      // points to Task's fiber-ptr slot; runtime fills it
};

struct cajeta_scope_frame {
    struct cajeta_scope_entry* entries;
    int count;
    int cap;
    struct cajeta_scope_frame* prev;
};

// Forward decls so cajeta_fiber can hold pointers to the drop and
// exception chain heads. Both chains are defined further down in this
// file (the file is ordered shared-infra first, then users).
struct cajeta_drop_entry;
struct cajeta_exception_frame;

// FiberLocal binding frame (full definition in the § FiberLocal section near
// the drop chain). Forward-declared here so the fiber control block can hold a
// per-fiber binding-stack head, in the same family as scope_top/drop_top/exc_top.
struct cajeta_fiber_local;

struct cajeta_fiber {
    ucontext_t ctx;
    void* stack;
    cajeta_fiber_state state;
    cajeta_task_trampoline_fn trampoline;
    void* trampoline_arg;
    struct cajeta_fiber* next;
    // Per-fiber scope chain. A naïve `__thread` would alias across fiber
    // switches (the carrier hosts many fibers on the same OS thread), so
    // each fiber owns its own stack of scope frames. Updated by
    // scope_enter/exit when invoked from a fiber context.
    struct cajeta_scope_frame* scope_top;
    // Per-fiber drop chain head and exception chain head. Same rationale
    // as scope_top: a single OS-thread-level `__thread` would alias
    // across fiber switches on the same carrier, so each fiber owns its
    // own chain. The main thread has its own `__thread` slot below;
    // __cajeta_drop_top_ptr / __cajeta_exc_top_ptr pick the right one
    // based on whether __cajeta_current_fiber is set.
    struct cajeta_drop_entry* drop_top;
    struct cajeta_exception_frame* exc_top;
    // FiberLocal binding stack head (docs/specification/concurrent/FiberLocal.md). Same per-fiber
    // rationale as scope_top/drop_top/exc_top: a __thread slot would alias across
    // the many fibers a carrier hosts. A fresh fiber inherits a deep-copied
    // snapshot of its spawner's chain (set in __cajeta_task_run); the chain is
    // freed at fiber teardown. The main thread uses __cajeta_main_fl_top below.
    struct cajeta_fiber_local* fl_top;
    // R5-C: cancellation marker. When non-NULL, the fiber's next
    // __cajeta_task_wait resume will throw this Throwable* instead of
    // returning normally. Set by __cajeta_fiber_cancel from scope's
    // first-throw escalation; cleared by the await re-raise path so
    // the same cancel doesn't fire twice on a fiber that survives.
    void* cancel_with;
    // C2: address of the Task's fiber-ptr slot (same as fiber_slot passed to
    // __cajeta_task_run). The carrier nulls *slot_ptr under __cajeta_task_mutex
    // before freeing the fiber, so a concurrent scope-cancel that reads the slot
    // (also under the mutex) never dereferences a freed fiber.
    void** slot_ptr;
    // Debugger CP3: stable per-fiber id for the DAP `threads`/`cajeta:fibers`
    // view and for stop events. Assigned from a monotonic counter at creation
    // (fibers get 1,2,3,...; the main thread reports id 0).
    int dbg_id;
    // Debugger CP5: per-fiber debug frame-chain head (locals capture).
    // Same aliasing rationale as scope_top/drop_top; selected by
    // __cajeta_dbg_top_ptr based on fiber-vs-main context.
    struct cajeta_dbg_frame* dbg_top;
    // Per-fiber frame arena (cajeta_rt_core.c). Same aliasing rationale as
    // scope_top/drop_top/exc_top: the arena's LIFO mark/reset discipline holds
    // per logical stack, and a carrier interleaves many fiber stacks — a
    // shared per-thread arena let one fiber's scope-exit reset reclaim a
    // PARKED fiber's live allocations (http:0.11). Lazily mapped on first
    // arena alloc; returned to the arena pool at fiber teardown.
    cajeta_arena arena;
    // Home carrier — the carrier that FIRST dispatched (started) this fiber.
    // -1 until then. Once a fiber has started, its saved `ucontext` (stack +
    // register state) is bound to that carrier; resuming it on a *different*
    // carrier is the unsolved cross-carrier handoff (corrupts on a multi-
    // carrier pool — see __cajeta_steal_one / __cajeta_publish_ready). So a
    // started fiber is pinned here: only its home carrier ever resumes it.
    // Fresh (not-yet-started) fibers have no saved context and may run on any
    // carrier — that's where the pool's parallelism comes from.
    int home_carrier;
};

static pthread_mutex_t __cajeta_task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __cajeta_task_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  __cajeta_task_done_cond  = PTHREAD_COND_INITIALIZER;

// FiberLocal helpers used by the fiber lifecycle below but defined in the
// § FiberLocal section (near the drop chain). Forward-declared so __cajeta_task_run
// can inherit the spawner's binding snapshot and the teardown path can free it.
static struct cajeta_fiber_local* __cajeta_fiber_local_snapshot_current(void);
static void __cajeta_fiber_local_free_chain(struct cajeta_fiber_local* head);

// R8.2 — per-carrier Chase-Lev work-stealing deque, replacing the prior
// linked-list ready queue. Single-producer (the owning carrier — i.e. the
// thread that called push_bottom / pop_bottom) and multi-consumer (other
// carriers stealing from the top, see deque_steal). For v1 there's still
// only one carrier, so the atomic protocol is dormant under
// __cajeta_task_mutex; the structure is in place so R8.3 (multi-carrier
// pool) lifts the owner-side mutex with minimal churn while the steal
// path is already correct.
//
// Layout: a fixed-size circular slot array plus monotonically-growing
// `top` / `bottom` sequence numbers. Bottom is where the owner pushes /
// pops (LIFO); top is where stealers consume (FIFO from the deque's
// perspective). The slot index is `seq % CAJETA_DEQUE_CAP`. Capacity is
// generous for v1; overflow aborts. A growable variant lands when a
// workload actually needs it.
//
// References: Chase & Lev 2005 "Dynamic Circular Work-Stealing Deque"; the
// memory-ordering choices here mirror the cppmem-validated lowering in
// the standard libcds / Crossbeam-deque implementations.
#define CAJETA_DEQUE_CAP 2048

struct cajeta_carrier_deque {
    // top / bottom are accessed via __atomic_* builtins (no _Atomic
    // qualifier — those builtins take plain integer pointers).
    int64_t top;
    int64_t bottom;
    struct cajeta_fiber* slots[CAJETA_DEQUE_CAP];
};

static void __cajeta_deque_init(struct cajeta_carrier_deque* d) {
    __atomic_store_n(&d->top, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&d->bottom, 0, __ATOMIC_SEQ_CST);
}

// Owner-side push (LIFO end). Single-writer; under the v1 mutex it's
// uncontended. Capacity overflow aborts — v1 doesn't grow.
static void __cajeta_deque_push_bottom(struct cajeta_carrier_deque* d,
                                        struct cajeta_fiber* f) {
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_RELAXED);
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_ACQUIRE);
    if (b - t >= CAJETA_DEQUE_CAP) {
        fprintf(stderr, "cajeta: carrier deque overflow (%lld slots in use)\n",
                (long long) (b - t));
        abort();
    }
    d->slots[b % CAJETA_DEQUE_CAP] = f;
    // Release the slot write before publishing the new bottom — a future
    // stealer observing the new `bottom` must also see the slot's content.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&d->bottom, b + 1, __ATOMIC_RELAXED);
}

// Owner-side pop (LIFO end). Returns NULL on empty. The Chase-Lev "last
// element" CAS is what makes the owner / stealer race tight even when
// only one fiber is left.
static struct cajeta_fiber* __cajeta_deque_pop_bottom(
        struct cajeta_carrier_deque* d) {
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_RELAXED) - 1;
    __atomic_store_n(&d->bottom, b, __ATOMIC_RELAXED);
    // The SeqCst fence pairs with steal()'s SeqCst fence; without it,
    // pop_bottom and steal can both think they got the last element.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_RELAXED);
    if (t > b) {
        // Empty — restore bottom and bail.
        __atomic_store_n(&d->bottom, t, __ATOMIC_RELAXED);
        return NULL;
    }
    struct cajeta_fiber* f = d->slots[b % CAJETA_DEQUE_CAP];
    if (t < b) {
        // Multi-element case — uncontended.
        return f;
    }
    // t == b: last element. Race with steal() for it.
    int64_t expected = t;
    if (!__atomic_compare_exchange_n(&d->top, &expected, t + 1,
            /*weak=*/0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        f = NULL;  // stealer won
    }
    __atomic_store_n(&d->bottom, t + 1, __ATOMIC_RELAXED);
    return f;
}

// Stealer-side pop (FIFO end). Returns NULL on empty or on a lost race
// (the owner / another stealer claimed the slot first). Unused by R8.2's
// single carrier — present so R8.3 lights it up without further deque
// surgery.
__attribute__((unused))
static struct cajeta_fiber* __cajeta_deque_steal(
        struct cajeta_carrier_deque* d) {
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_ACQUIRE);
    // Pairs with pop_bottom's SeqCst fence.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_ACQUIRE);
    if (t >= b) return NULL;
    struct cajeta_fiber* f = d->slots[t % CAJETA_DEQUE_CAP];
    int64_t expected = t;
    if (!__atomic_compare_exchange_n(&d->top, &expected, t + 1,
            /*weak=*/0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        return NULL;  // lost the race
    }
    return f;
}

// Snapshot the deque's size from the owner's perspective. Callers hold
// __cajeta_task_mutex so this is the only writer running; the load
// orderings can be relaxed.
static int64_t __cajeta_deque_size(struct cajeta_carrier_deque* d) {
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_RELAXED);
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_RELAXED);
    return b - t;
}
// Parked list: fibers blocked inside __cajeta_task_wait waiting for some
// task's done flag to flip. Wake-all-on-any-complete is the v1 strategy.
// Protected by __cajeta_task_mutex (the pool-level mutex).
static struct cajeta_fiber* __cajeta_parked_head = NULL;
static int __cajeta_task_workers_started = 0;
// Set by __cajeta_task_shutdown to signal the carrier loop to exit.
// The carrier's pthread_cond_wait predicate checks this alongside
// the ready-queue head so a shutdown request reliably unblocks it.
static int __cajeta_task_shutdown_requested = 0;

// R9.1 timer state (declared here so __cajeta_task_shutdown — which lives
// above the timer implementation block — can join the timer thread on
// teardown). The struct cajeta_timer_entry definition + the function
// bodies live further down alongside __cajeta_task_wait_timeout.
struct cajeta_timer_entry;
static struct cajeta_timer_entry* __cajeta_timer_head = NULL;
static pthread_cond_t __cajeta_timer_cond = PTHREAD_COND_INITIALIZER;
static pthread_t __cajeta_timer_thread;
static int __cajeta_timer_started = 0;
static int __cajeta_timer_shutdown_requested = 0;

// R9.4 reactor state (declared above task_shutdown for the same reason as
// the timer state). The waiter struct + reactor loop body live below
// alongside __cajeta_io_wait.
struct cajeta_io_waiter;
static struct cajeta_io_waiter* __cajeta_reactor_waiters = NULL;
static pthread_t __cajeta_reactor_thread;
static int __cajeta_reactor_started = 0;
static int __cajeta_reactor_shutdown_requested = 0;
#if defined(__linux__)
static int __cajeta_reactor_epfd = -1;
#endif

// NET-3.2 — net-reactor lifecycle teardown hook (defined in
// cajeta_net_reactor_lifecycle.c, #included at the bottom of this TU). Forward-
// declared here so __cajeta_task_shutdown can drain + close the net reactor's
// own lifecycle state (the `started` latch, the live-registration balance, the
// shutdown wake pipe) once the carriers + R9.4 reactor thread are joined. It is
// idempotent and a cheap no-op when no awaitable net op ever ran.
int32_t __cajeta_net_reactor_shutdown(void);

// R8.3 — multi-carrier pool, flag-gated N=1.
//
// Each carrier owns one Chase-Lev deque + a deque_mutex that protects
// non-steal accesses to it (push_bottom / pop_bottom). The deque's steal
// path stays lock-free, so other carriers can steal from this carrier's
// top without taking deque_mutex — only the owner-side ops (push_bottom,
// pop_bottom) serialize on it. Cross-thread pushes (main-thread spawn,
// lock_release waking a waiter, etc.) take the target carrier's deque_mutex
// — Chase-Lev's single-producer contract holds within the mutex.
//
// Pool-level coordination — sleep/wake of idle carriers, shutdown,
// parked-list bookkeeping — runs under __cajeta_task_mutex
// (pre-existing). One shared cond, __cajeta_task_queue_cond, that idle
// carriers cond_wait on; pushers signal one when sleeping_count > 0
// (broadcast on shutdown).
//
// Carrier count is read from $CAJETA_CARRIERS at the first
// __cajeta_task_run, clamped to [1, CAJETA_MAX_CARRIERS]. Default is
// min(_SC_NPROCESSORS_ONLN, CAJETA_DEFAULT_CARRIERS_CAP) — multi-carrier
// by default so spawned tasks actually run in parallel. Single-carrier
// behaviour the prior releases had is opt-in via CAJETA_CARRIERS=1
// (still the canonical knob for deterministic-order debug runs).
#define CAJETA_MAX_CARRIERS 16
// Default-cap so a 64-core box doesn't spin up 16 carriers on a
// program that has no work for them. Empirically picked at 4 — gives
// real parallelism for the typical channel/fan-out workloads in tests
// and stays comfortably inside CAJETA_MAX_CARRIERS' upper bound. Users
// who want more can set CAJETA_CARRIERS=N explicitly.
#define CAJETA_DEFAULT_CARRIERS_CAP 4

struct cajeta_carrier {
    pthread_t thread;
    int carrier_id;
    pthread_mutex_t deque_mutex;
    struct cajeta_carrier_deque deque;
};

static struct cajeta_carrier __cajeta_carriers[CAJETA_MAX_CARRIERS];
static int __cajeta_carrier_count = 0;
// Number of carriers currently parked in cond_wait. Pushers consult
// this to decide whether to signal — non-zero means a signal will
// land on a sleeper rather than burning cycles on a busy one.
static int __cajeta_sleeping_count = 0;

// Per-thread pointer to the running carrier struct. NULL on the main
// thread / any non-carrier thread. Used by __cajeta_publish_ready to
// route new work onto the spawning carrier's own deque (locality —
// the carrier that produced the work is likely to consume it next).
static __thread struct cajeta_carrier* __cajeta_my_carrier = NULL;

// Per-OS-thread state. Only the carrier thread will ever have a non-null
// __cajeta_current_fiber — the main thread sees it null and falls through
// to the cond_wait path in __cajeta_task_wait.
static __thread struct cajeta_fiber* __cajeta_current_fiber = NULL;

// Frame-arena context selector (forward-declared in cajeta_rt_core.c, same
// TU): the running fiber's own arena, else the thread's. Mirrors
// __cajeta_scope_top_ptr / __cajeta_drop_top_ptr below.
static cajeta_arena* __cajeta_arena_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->arena;
    }
    return &__cajeta_arena;
}
static __thread ucontext_t __cajeta_carrier_ctx;

// Scope chain for code running outside a fiber (the main entry point, or
// any pthread that isn't a carrier-hosted fiber). Fibers carry their own
// scope chain in `cajeta_fiber::scope_top`; the helper below picks the
// right one based on current context.
static __thread struct cajeta_scope_frame* __cajeta_main_scope_top = NULL;

// Debugger CP5: main/program-thread slot for the dbg frame chain (the JIT
// entry runs on a plain bg thread, not a carrier fiber, so it uses this).
static __thread struct cajeta_dbg_frame* __cajeta_main_dbg_top = NULL;

// Returns a pointer to the current scope_top slot — either the running
// fiber's slot or the main thread's TLS slot. Callers read or write
// through this pointer, so push/pop works uniformly regardless of
// fiber vs main context.
static struct cajeta_scope_frame** __cajeta_scope_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->scope_top;
    }
    return &__cajeta_main_scope_top;
}

// Debugger CP5: selector for the dbg frame-chain head, mirroring
// __cajeta_scope_top_ptr. Forward-declared up in the safepoint section.
struct cajeta_dbg_frame** __cajeta_dbg_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->dbg_top;
    }
    return &__cajeta_main_dbg_top;
}

// Debugger CP3: id of the fiber running on this carrier thread, or 0 when not
// in a fiber (the main thread / program entry). Forward-declared up in the
// debug-safepoint section; defined here where __cajeta_current_fiber is in
// scope.
int __cajeta_dbg_current_fiber_id(void) {
    return __cajeta_current_fiber ? __cajeta_current_fiber->dbg_id : 0;
}

// Debugger CP6f-2: stateless per-fiber accessors. Like the frame-chain
// accessors above, these cast an opaque handle (from __cajeta_dbg_fiber_at)
// back to the fiber struct so the host's NATIVE runtime copy can read a fiber
// the JIT copy registered (both copies lay the struct out identically).
// dbg_id is the stable per-fiber id; frame_top is the head of that fiber's
// debug frame chain (feed it to DebugVars::walkFrames); state is the
// cajeta_fiber_state enum value.
long __cajeta_dbg_fiber_id_of(void* fiber) {
    return fiber ? (long) ((struct cajeta_fiber*) fiber)->dbg_id : 0;
}

void* __cajeta_dbg_fiber_frame_top(void* fiber) {
    return fiber ? ((struct cajeta_fiber*) fiber)->dbg_top : NULL;
}

int __cajeta_dbg_fiber_state(void* fiber) {
    return fiber ? (int) ((struct cajeta_fiber*) fiber)->state : -1;
}

// Fiber entry trampoline — invoked by makecontext on first resume. Runs
// the user trampoline (which calls the async fn + signals done), then
// explicitly swaps back to the running carrier's TLS slot.
//
// R8.3 — explicit swap-back instead of relying on uc_link. uc_link is a
// pointer baked into the ucontext at makecontext time, and on glibc
// `&__cajeta_carrier_ctx` resolves to the carrier-thread that
// FIRST DISPATCHED the fiber. After a steal, a different carrier owns
// the fiber but uc_link still points at the original carrier's TLS slot;
// a fall-off-the-end then jumps into the wrong saved context. The
// explicit swapcontext here evaluates `&__cajeta_carrier_ctx` in the
// CURRENTLY running carrier's TLS, so it's always the right address.
static void __cajeta_fiber_entry(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    f->trampoline(f->trampoline_arg);
    f->state = CAJETA_FIBER_DONE;
    __cajeta_swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// R8.3 — publish a ready fiber onto one of the pool's deques. Routes to
// the current carrier's deque when the caller is running on one (locality);
// otherwise carrier 0 (main-thread spawn, lock-release on the main thread,
// etc.). Takes the target carrier's deque_mutex around the push so
// Chase-Lev's single-producer invariant holds, then signals one sleeper on
// the pool condvar if any carrier is parked.
//
// Caller must NOT hold __cajeta_task_mutex; this function takes it
// internally for the sleeper-signal step. (Holding it would deadlock
// against this routine's own attempt to acquire it.)
static void __cajeta_publish_ready(struct cajeta_fiber* f) {
    f->state = CAJETA_FIBER_READY;
    f->next = NULL;
    // A fiber that has already started is pinned to its home carrier — its
    // saved ucontext can only be resumed there. Route it home regardless of
    // who is waking it. A fresh fiber (home_carrier < 0) has no saved context
    // yet, so route it to the waker's own carrier for locality (it may still be
    // stolen by an idle peer — safe, since it makecontext's on the stealer).
    struct cajeta_carrier* target;
    if (f->home_carrier >= 0) {
        target = &__cajeta_carriers[f->home_carrier];
    } else {
        target = __cajeta_my_carrier ? __cajeta_my_carrier
                                     : &__cajeta_carriers[0];
    }
    pthread_mutex_lock(&target->deque_mutex);
    __cajeta_deque_push_bottom(&target->deque, f);
    pthread_mutex_unlock(&target->deque_mutex);
    pthread_mutex_lock(&__cajeta_task_mutex);
    if (__cajeta_sleeping_count > 0) {
        pthread_cond_signal(&__cajeta_task_queue_cond);
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// Detach every parked fiber and return them as a NULL-terminated list.
// Caller holds __cajeta_task_mutex. The caller is responsible for
// publishing each one onto a carrier's deque AFTER releasing the pool
// mutex — publishing under the pool mutex would invert the lock order
// publish_ready uses (deque → pool) and deadlock with concurrent pushes.
static struct cajeta_fiber* __cajeta_drain_parked_locked(void) {
    struct cajeta_fiber* drained = __cajeta_parked_head;
    __cajeta_parked_head = NULL;
    return drained;
}

// Total work across every carrier's deque. Lock-free — reads atomic
// top/bottom on each deque without taking deque mutexes. Used by carriers
// pre-sleep to avoid going to cond_wait when there's still work somewhere
// in the pool (the race-fix for the small window between a steal-scan
// finishing empty and the carrier committing to sleep).
static int64_t __cajeta_pool_total_work(void) {
    int64_t total = 0;
    for (int i = 0; i < __cajeta_carrier_count; ++i) {
        total += __cajeta_deque_size(&__cajeta_carriers[i].deque);
    }
    return total;
}

// Steal a ready fiber from a peer carrier's deque top. Round-robin starting
// after `self` to spread the steal load. NULL if every peer's deque is
// empty or every steal attempt loses the CAS race.
static struct cajeta_fiber* __cajeta_steal_one(struct cajeta_carrier* self) {
    int n = __cajeta_carrier_count;
    if (n <= 1) return NULL;
    int start = self ? self->carrier_id + 1 : 0;
    for (int step = 0; step < n; ++step) {
        int idx = (start + step) % n;
        if (&__cajeta_carriers[idx] == self) continue;
        struct cajeta_fiber* f = __cajeta_deque_steal(&__cajeta_carriers[idx].deque);
        if (!f) continue;
        // Only fresh fibers (no home yet) or fibers already homed to us may run
        // here. A started fiber pinned to another carrier carries a live
        // ucontext bound to that carrier — resuming it on `self` is the
        // cross-carrier handoff that corrupts. Hand it back to its home deque
        // (waking that carrier if it's parked) and keep scanning.
        if (f->home_carrier >= 0 && f->home_carrier != self->carrier_id) {
            struct cajeta_carrier* home = &__cajeta_carriers[f->home_carrier];
            pthread_mutex_lock(&home->deque_mutex);
            __cajeta_deque_push_bottom(&home->deque, f);
            pthread_mutex_unlock(&home->deque_mutex);
            pthread_mutex_lock(&__cajeta_task_mutex);
            if (__cajeta_sleeping_count > 0) {
                pthread_cond_broadcast(&__cajeta_task_queue_cond);
            }
            pthread_mutex_unlock(&__cajeta_task_mutex);
            continue;
        }
        return f;
    }
    return NULL;
}

// Park the running fiber. Called by __cajeta_task_wait when inside a fiber
// and the awaited task isn't done. Adds the fiber to parked_head, then
// swaps back to the carrier — the swap returns into this function only
// after the fiber gets woken and the carrier dispatches it again.
static void __cajeta_fiber_park(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    pthread_mutex_lock(&__cajeta_task_mutex);
    f->state = CAJETA_FIBER_PARKED;
    f->next = __cajeta_parked_head;
    __cajeta_parked_head = f;
    pthread_mutex_unlock(&__cajeta_task_mutex);
    __cajeta_swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// Park variant for callers that ALREADY hold __cajeta_task_mutex. Enqueues
// the fiber on parked_head and releases the mutex BEFORE swapping to the
// carrier. The point is atomicity: a caller rechecks its wake condition
// (done flag, deadline, I/O registration) under the same mutex immediately
// before calling this, so the fiber is committed to parked_head before any
// concurrent waker — task_complete on another carrier, the reactor thread,
// the timer thread — can acquire the mutex and drain/remove it. Without
// this, the unlocked-check-then-separately-lock-and-park sequence has a
// lost-wakeup window: the waker fires between the check and the enqueue,
// finds nothing parked, and the fiber then parks forever. (Single-carrier
// runs never hit it — everything is cooperative on one OS thread — which is
// why the await/deadline/io tests only hang under the multi-carrier pool.)
// Home-carrier pinning makes the post-unlock pre-swap window safe: only this
// fiber's home carrier resumes it, and that is the very thread executing the
// swap, so it cannot be re-dispatched until the swap has saved its context.
static void __cajeta_fiber_park_locked(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    f->state = CAJETA_FIBER_PARKED;
    f->next = __cajeta_parked_head;
    __cajeta_parked_head = f;
    pthread_mutex_unlock(&__cajeta_task_mutex);
    __cajeta_swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// Carrier loop. Pop own deque (LIFO, cache-warm); on empty, try stealing
// from peer carriers (lock-free); on universally-empty, sleep on the pool
// condvar until a pusher signals. R8.3 — the carrier struct comes in via
// the pthread_create arg so each carrier knows its own deque, mutex, and
// id without TLS-init plumbing.
static void* __cajeta_carrier_loop(void* arg) {
    struct cajeta_carrier* self = (struct cajeta_carrier*) arg;
    __cajeta_my_carrier = self;
    for (;;) {
        // Owner-side pop on self's deque. Under self->deque_mutex so a
        // concurrent cross-thread push (publish_ready from another
        // carrier or main) can't race with this pop.
        pthread_mutex_lock(&self->deque_mutex);
        struct cajeta_fiber* f = __cajeta_deque_pop_bottom(&self->deque);
        pthread_mutex_unlock(&self->deque_mutex);

        // Empty own deque — try stealing from peers (lock-free).
        if (!f) {
            f = __cajeta_steal_one(self);
        }

        if (!f) {
            // Pool-wide empty. Decide whether to exit, retry, or wait — all
            // off a SINGLE pool_total_work() read taken under the pool mutex.
            //
            // The earlier form called pool_total_work() twice (once for the
            // shutdown-exit check, once for the retry check). When work
            // transiently read >0 then 0 across those two calls, a carrier
            // that had ALREADY observed shutdown_requested fell through to
            // cond_wait — but the shutdown broadcast is one-shot and had
            // already fired, so that carrier never woke and __cajeta_task_
            // shutdown's join() wedged forever. Reading work once closes the
            // TOCTOU; checking shutdown FIRST guarantees a shutting-down
            // carrier never blocks on the one-shot condvar (it exits when the
            // pool is drained, else loops to drain reachable work). The wait
            // is also bounded as a final backstop against any missed wake on
            // the work path — the broadcast/signal still wakes us immediately
            // in the common case; the timeout only matters if a wake is lost.
            pthread_mutex_lock(&__cajeta_task_mutex);
            int64_t work = __cajeta_pool_total_work();
            if (__cajeta_task_shutdown_requested) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                if (work == 0) {
                    return NULL;
                }
                continue;  // drain reachable work; never cond_wait at shutdown
            }
            if (work > 0) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                continue;
            }
            // CP6f-2d: an idle carrier under a debugger stop is already not
            // executing Cajeta — count it as quiesced and park until resume
            // (spec §2.2.4) instead of sleeping on the no-work condvar, so the
            // barrier converges and the carrier can't grab work mid-stop. Park
            // OUTSIDE the task mutex (lock order: stop_mu is a leaf).
            if (__cajeta_stop_is_requested()) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                __cajeta_stop_park();
                continue;
            }
            __cajeta_sleeping_count++;
            struct timespec __wait_ts;
            clock_gettime(CLOCK_REALTIME, &__wait_ts);
            __wait_ts.tv_nsec += 50 * 1000 * 1000;  // 50ms backstop
            if (__wait_ts.tv_nsec >= 1000000000L) {
                __wait_ts.tv_nsec -= 1000000000L;
                __wait_ts.tv_sec += 1;
            }
            pthread_cond_timedwait(&__cajeta_task_queue_cond, &__cajeta_task_mutex, &__wait_ts);
            __cajeta_sleeping_count--;
            pthread_mutex_unlock(&__cajeta_task_mutex);
            continue;
        }
        f->next = NULL;

        // CP6f-2d: scheduler hand-off stop check (spec §2.2.3). If a debugger
        // stop is in flight, park this carrier BEFORE running the next fiber so
        // a carrier between fibers can't start new Cajeta work while the world
        // is stopped. The popped fiber `f` stays in hand and runs after resume.
        if (__cajeta_stop_is_requested()) __cajeta_stop_park();

        __cajeta_current_fiber = f;
        f->state = CAJETA_FIBER_RUNNING;
        if (!f->stack) {
            // First-resume init: allocate the stack and prime the context
            // to dispatch to __cajeta_fiber_entry. uc_link points back at
            // this thread's carrier_ctx slot — single carrier per thread,
            // so the address stays stable across the fiber's lifetime.
            // For multi-carrier work-stealing (R8.3+), a fiber CAN land
            // on a different carrier than originally allocated; uc_link
            // would then need updating before each swap, but the obvious
            // `f->ctx.uc_link = &__cajeta_carrier_ctx` immediately
            // before swapcontext corrupts the saved trampoline state on
            // glibc (probably because makecontext snapshots uc_link
            // arch-dependently into the synthesized frame). v1 punts:
            // stealing is disabled below until the cross-carrier
            // uc_link handoff is solved at a deeper layer.
            size_t stack_size = __cajeta_fiber_stack_alloc_size();
            f->stack = __cajeta_fiber_stack_alloc();   // guard-paged on POSIX
            // First dispatch: this carrier becomes the fiber's home. From here
            // on the fiber's saved ucontext is bound to this carrier, so only
            // this carrier may resume it (steal/publish honor home_carrier).
            f->home_carrier = self->carrier_id;
            __cajeta_getcontext(&f->ctx);
            f->ctx.uc_stack.ss_sp = f->stack;
            f->ctx.uc_stack.ss_size = stack_size;
            f->ctx.uc_link = &__cajeta_carrier_ctx;
            __cajeta_makecontext(&f->ctx, __cajeta_fiber_entry, 0);
        }
        __cajeta_swapcontext(&__cajeta_carrier_ctx, &f->ctx);
        __cajeta_current_fiber = NULL;
        if (f->state == CAJETA_FIBER_DONE) {
            // C2: null the Task's fiber slot before freeing, under the task mutex,
            // so a concurrent scope-cancel reading the slot (also under the mutex)
            // sees NULL instead of a dangling fiber pointer.
            pthread_mutex_lock(&__cajeta_task_mutex);
            if (f->slot_ptr) *f->slot_ptr = NULL;
            pthread_mutex_unlock(&__cajeta_task_mutex);
            // Debugger CP6f-2: drop from the live-fiber registry before freeing
            // so the fibers view never hands the host a dangling handle.
            __cajeta_dbg_fiber_unregister(f);
#if defined(_WIN32)
            // H19: on Windows the fiber is a Win32 fiber object (CreateFiber); the
            // free() below reclaims the struct but not the OS fiber. Delete it
            // (safe here — the fiber has returned to the carrier, it isn't current)
            // to avoid leaking one OS fiber + its stack per completed task.
            if (f->ctx.fiber) DeleteFiber(f->ctx.fiber);
#endif
            // Free any FiberLocal frames still linked (inherited snapshot copies,
            // plus any unbalanced set/push the body left — where() pops its own).
            __cajeta_fiber_local_free_chain(f->fl_top);
            f->fl_top = NULL;
            // Recycle the fiber's frame-arena mapping (no-op if never used).
            __cajeta_arena_release_mapping(&f->arena);
            __cajeta_fiber_stack_free(f->stack);
            free(f);
        }
        // Parked fibers stay on __cajeta_parked_head awaiting a wake.
    }
    return NULL;
}

// Signal the carrier thread to exit and join it. Used by JIT-mode test
// teardown so test 1's carrier doesn't survive into test 2 (each
// __cajeta_task_run lazy-starts a fresh carrier bound to its module's
// statics; without shutdown, test 1's carrier is left waiting on a
// condvar whose backing memory the JIT will recycle or unmap, and the
// next process-wide signal — typically test 2's cond_broadcast on the
// remapped condvar address — wakes that orphaned carrier into JIT code
// that no longer exists). Safe to call when no carrier was started:
// the flag check makes it a no-op.
// CP6f-2d: number of carrier threads in the current pool (0 if not started).
// The debugger's quiesce barrier uses this to compute how many carriers must
// park before inspection (expected = active carriers minus the primary).
int __cajeta_carrier_count_get(void) {
    return __cajeta_carrier_count;
}

void __cajeta_task_shutdown(void) {
    pthread_mutex_lock(&__cajeta_task_mutex);
    if (!__cajeta_task_workers_started) {
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return;
    }
    __cajeta_task_shutdown_requested = 1;
    pthread_cond_broadcast(&__cajeta_task_queue_cond);
    int n = __cajeta_carrier_count;
    // R9.1 — if the timer thread was lazy-started, signal it to exit too.
    // Same JIT-survives-across-tests rationale as carrier shutdown: leaving
    // it running would let it observe (and signal on) a recycled condvar
    // address in the next test.
    int timer_was_started = __cajeta_timer_started;
    if (timer_was_started) {
        __cajeta_timer_shutdown_requested = 1;
        pthread_cond_signal(&__cajeta_timer_cond);
    }
    // R9.4 — same for the I/O reactor. It uses epoll_wait with a 1s
    // poll timeout, so the shutdown flag is observed within ~1s without
    // any explicit wake. Closing the epfd from outside would race the
    // reactor's epoll_wait return, so we let the poll timeout do it.
    int reactor_was_started = __cajeta_reactor_started;
    if (reactor_was_started) {
        __cajeta_reactor_shutdown_requested = 1;
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
    for (int i = 0; i < n; ++i) {
        pthread_join(__cajeta_carriers[i].thread, NULL);
    }
    if (timer_was_started) {
        pthread_join(__cajeta_timer_thread, NULL);
    }
    if (reactor_was_started) {
        pthread_join(__cajeta_reactor_thread, NULL);
    }
    // Reset state so a subsequent __cajeta_task_run (e.g. the next test's
    // first spawn) starts a fresh pool with clean queues. The deque per
    // carrier is re-initialized on the next workers_started=1 transition
    // in __cajeta_task_run.
    pthread_mutex_lock(&__cajeta_task_mutex);
    // H6: free any fibers still parked (one never woken) and null the head.
    // Otherwise their structs + 64KB stacks leak, and worse, a stale parked
    // fiber surviving into the next run would be re-readied by that run's first
    // task_complete and swapcontext'd into a recycled/unmapped JIT context.
    // (Runnable fibers live in the per-carrier deques under main's work-stealing
    // scheduler — there is no global ready queue to drain here.)
    for (struct cajeta_fiber* f = __cajeta_parked_head; f; ) {
        struct cajeta_fiber* nx = f->next;
        if (f->slot_ptr) *f->slot_ptr = NULL;
        __cajeta_arena_release_mapping(&f->arena);
        __cajeta_fiber_stack_free(f->stack); free(f); f = nx;
    }
    __cajeta_parked_head = NULL;
    __cajeta_task_shutdown_requested = 0;
    __cajeta_task_workers_started = 0;
    for (int i = 0; i < n; ++i) {
        pthread_mutex_destroy(&__cajeta_carriers[i].deque_mutex);
    }
    __cajeta_carrier_count = 0;
    if (timer_was_started) {
        __cajeta_timer_started = 0;
        __cajeta_timer_shutdown_requested = 0;
        __cajeta_timer_head = NULL;
    }
    if (reactor_was_started) {
        __cajeta_reactor_started = 0;
        __cajeta_reactor_shutdown_requested = 0;
        __cajeta_reactor_waiters = NULL;
#if defined(__linux__)
        if (__cajeta_reactor_epfd >= 0) {
            close(__cajeta_reactor_epfd);
            __cajeta_reactor_epfd = -1;
        }
#endif
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);

    // NET-3.2 — tear down the net-reactor lifecycle (separate from the R9.4
    // engine above): wake any portable-path waiter, drain the live-registration
    // balance, reset the lazy-init latch, and close the shutdown wake pipe. Done
    // OUTSIDE __cajeta_task_mutex — it takes its own dedicated lifecycle mutex,
    // and keeping the two lock domains disjoint avoids any ordering coupling.
    // Idempotent + a no-op when no awaitable net op ever initialized it.
    __cajeta_net_reactor_shutdown();
}

// Enqueue a trampoline-arg pair as a fresh fiber. The actual stack +
// ucontext init is deferred to the carrier's first dispatch so cancelled-
// before-resumed spawns don't pay the stack-alloc cost. Lazy carrier
// thread start mirrors R2: programs that never spawn don't pay for a
// pthread_create. `fiber_slot` (R5-C) is the address of the Task's
// FIBER_FIELD slot — we write the freshly-allocated fiber's pointer
// there so scope can find the fiber by walking its registered task list.
void __cajeta_task_run(void* arg, cajeta_task_trampoline_fn trampoline,
                       void** fiber_slot) {
    struct cajeta_fiber* f = malloc(sizeof(*f));
    if (!f) {
        fprintf(stderr, "cajeta: __cajeta_task_run fiber malloc failed\n");
        abort();
    }
    f->ctx = (ucontext_t) {0};
    f->stack = NULL;
    f->state = CAJETA_FIBER_READY;
    f->trampoline = trampoline;
    f->trampoline_arg = arg;
    f->next = NULL;
    f->scope_top = NULL;
    f->drop_top = NULL;
    f->exc_top = NULL;
    f->cancel_with = NULL;
    f->slot_ptr = fiber_slot;   // C2: so the carrier can null it before free
    f->dbg_top = NULL;
    // Inherit-on-spawn (FiberLocal Layer 2): a deep-copied snapshot of the
    // SPAWNER's binding chain. __cajeta_task_run runs on the spawner's context,
    // so __cajeta_current_fiber (read inside snapshot_current) is the spawner
    // — or NULL on the main thread, snapshotting the main __thread chain. A
    // deep copy (not a shared pointer) keeps the child's lifetime independent of
    // the spawner's pop order, which matters for detach as well as structured
    // spawn; child where()s push fresh frames, never mutating an inherited one.
    f->fl_top = __cajeta_fiber_local_snapshot_current();
    f->arena = (cajeta_arena) { NULL, 0, 0, 0, 0 };  // lazily mapped on first use
    f->home_carrier = -1;   // assigned on first dispatch (see carrier_loop)
    if (fiber_slot) *fiber_slot = f;

    pthread_mutex_lock(&__cajeta_task_mutex);
    // Debugger CP3/CP6f-2: assign a stable id (fibers get 1,2,3,...; the
    // program/main thread reports id 0) and add to the live-fiber registry so
    // the DAP fibers view can enumerate it. (CP3 declared dbg_id but never
    // assigned it, so every fiber reported 0 — fixed here.) register() locks
    // the reg mutex INSIDE the task mutex; nothing locks the other order.
    f->dbg_id = (int) ++__cajeta_dbg_fiber_id_counter;
    __cajeta_dbg_fiber_register(f);
    if (!__cajeta_task_workers_started) {
        __cajeta_task_workers_started = 1;
        // Carrier count from $CAJETA_CARRIERS if set; otherwise default
        // to min(_SC_NPROCESSORS_ONLN, CAJETA_DEFAULT_CARRIERS_CAP) so
        // spawned tasks get real parallelism out of the box. Clamped to
        // [1, CAJETA_MAX_CARRIERS]. Read once at first spawn — the pool
        // is fixed-size for the lifetime of this scheduler instance.
        int n;
        const char* env = getenv("CAJETA_CARRIERS");
        if (env && *env) {
            int parsed = atoi(env);
            n = (parsed >= 1) ? parsed : 1;
        } else {
#if defined(_WIN32)
            // sysconf/_SC_NPROCESSORS_ONLN is POSIX; on Windows ask the Win32
            // API (windows.h is included at file scope for the fiber/lock paths).
            SYSTEM_INFO cpu_si;
            GetSystemInfo(&cpu_si);
            long cores = (long) cpu_si.dwNumberOfProcessors;
#else
            long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
            if (cores < 1) cores = 1;
            n = (int) cores;
            if (n > CAJETA_DEFAULT_CARRIERS_CAP) n = CAJETA_DEFAULT_CARRIERS_CAP;
        }
        if (n > CAJETA_MAX_CARRIERS) n = CAJETA_MAX_CARRIERS;
        __cajeta_carrier_count = n;
        for (int i = 0; i < n; ++i) {
            __cajeta_carriers[i].carrier_id = i;
            pthread_mutex_init(&__cajeta_carriers[i].deque_mutex, NULL);
            __cajeta_deque_init(&__cajeta_carriers[i].deque);
        }
        // Second-thread barrier: switch the live-set to its locked path before
        // any carrier can allocate (release-ordered, on the main thread).
        __cajeta_live_set_go_multithreaded();
        for (int i = 0; i < n; ++i) {
            pthread_create(&__cajeta_carriers[i].thread, NULL,
                           __cajeta_carrier_loop, &__cajeta_carriers[i]);
        }
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
    // Publish the new fiber. publish_ready acquires the target carrier's
    // deque_mutex itself; doing it outside __cajeta_task_mutex keeps the
    // pool → deque lock order (and avoids the recursive task_mutex grab
    // publish_ready does for the sleeper-signal step).
    __cajeta_publish_ready(f);
}

// Block until the task at `done_addr` flips to nonzero. From a fiber:
// park-yield-recheck; the carrier can run other fibers in between. From
// the main thread (no current fiber): condvar wait. This is what makes
// nested await work — a fiber awaiting another fiber doesn't hold the
// carrier hostage.
//
// R5-C: after each fiber wake, check the fiber's cancel_with marker.
// If set, the surrounding scope cancelled us — throw the trigger so
// the cancelled fiber's body unwinds and the trampoline catches it
// onto the Task's exception slot (where scope's next walk picks it up,
// but for cancellation siblings that's just a propagation of what the
// scope already decided to raise).
void __cajeta_task_wait(int32_t* done_addr) {
    if (!done_addr) return;
    if (__cajeta_current_fiber) {
        // C2: deliver a pending cancellation even when the awaited task is
        // ALREADY done: the loop below only re-checks cancel_with after a park,
        // which never happens if *done_addr is set on entry — so without this the
        // scope's cancel would be silently swallowed and the fiber run on.
        void* pending = __cajeta_current_fiber->cancel_with;
        if (pending) {
            __cajeta_current_fiber->cancel_with = NULL;
            __cajeta_throw(pending);
        }
        // Recheck *done_addr under the SAME mutex task_complete uses to set
        // it and drain parked fibers, then park atomically. If done flipped
        // before we acquired the lock, we see it here and never park — which
        // closes the lost-wakeup window (task_complete draining an empty
        // parked list, then this fiber parking with no future waker).
        for (;;) {
            pthread_mutex_lock(&__cajeta_task_mutex);
            if (*done_addr) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                break;
            }
            __cajeta_fiber_park_locked();  // releases the mutex, then swaps
            void* cancel = __cajeta_current_fiber->cancel_with;
            if (cancel) {
                __cajeta_current_fiber->cancel_with = NULL;
                __cajeta_throw(cancel);
            }
        }
        return;
    }
    pthread_mutex_lock(&__cajeta_task_mutex);
    while (!*done_addr) {
        pthread_cond_wait(&__cajeta_task_done_cond, &__cajeta_task_mutex);
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// R5-C: set a fiber's cancel_with marker. Its next __cajeta_task_wait
// resume will throw the marker instead of returning normally. Idempotent —
// re-cancel just overwrites the marker (last cancel wins). NULL fiber
// is a no-op (caller may not have a fiber pointer if the task hasn't
// been dispatched yet — but cancel_with set on a not-yet-dispatched
// fiber is still honored as soon as it parks on its first await).
void __cajeta_fiber_cancel(struct cajeta_fiber* fiber, void* throwable) {
    if (!fiber) return;
    fiber->cancel_with = throwable;
}

// Called by the codegen-emitted trampoline once the inner fn has run and
// its result has been written into the task's value slot. Sets done = 1
// under the mutex, wakes any main-thread awaiter via cond_done, and moves
// every parked fiber back to the ready queue so they can recheck their
// own await condition.
void __cajeta_task_complete(int32_t* done_addr) {
    if (!done_addr) return;
    // Phase 1 — flip the done flag, wake any main-thread awaiters, and
    // detach the parked list (under pool_mutex only).
    pthread_mutex_lock(&__cajeta_task_mutex);
    // Null the Task's fiber slot BEFORE publishing done. The moment
    // *done_addr = 1 becomes visible, the awaiter can return from
    // __cajeta_task_wait and the Task can be dropped + freed — so the
    // runtime must never touch Task memory after this point. The carrier's
    // post-swap cleanup used to perform this null AFTER the done signal,
    // and under CPU oversubscription (carrier preempted between the signal
    // and the cleanup) that write landed in freed/recycled heap: the
    // corrupted-size/SIGSEGV crashes the parallel suites hit under load.
    // Doing it here, under the same mutex scope-cancel takes, preserves
    // C2's invariant: a concurrent cancel sees the live fiber or NULL,
    // never a dangling pointer. slot_ptr is also cleared on the fiber so
    // the carrier's (now redundant) backstop null is a no-op.
    struct cajeta_fiber* self = __cajeta_current_fiber;
    if (self && self->slot_ptr) {
        *self->slot_ptr = NULL;
        self->slot_ptr = NULL;
    }
    *done_addr = 1;
    pthread_cond_broadcast(&__cajeta_task_done_cond);
    struct cajeta_fiber* woken = __cajeta_drain_parked_locked();
    pthread_mutex_unlock(&__cajeta_task_mutex);
    // Phase 2 — publish each woken parker WITHOUT holding pool_mutex.
    // publish_ready's lock order (deque → pool) would invert against
    // pool → deque if we did this inside Phase 1.
    while (woken) {
        struct cajeta_fiber* next = woken->next;
        __cajeta_publish_ready(woken);
        woken = next;
    }
}

// --- R9.1 — timer wheel + cooperative timeout -----------------------------
//
// Goal: let a fiber's __cajeta_task_wait honor a deadline. The intrinsic
// __cajeta_task_wait_timeout returns 1 if *done_addr flips before the
// deadline, 0 if the deadline expires first.
//
// Mechanism: a sorted singly-linked list of (deadline_ns, fiber) entries
// protected by __cajeta_task_mutex; a single timer thread (lazy-started on
// the first registration) sleeps via pthread_cond_timedwait until the next
// deadline; on expire it walks the list, detaches expired fibers from
// __cajeta_parked_head and publishes them. The fiber-side loop rechecks
// done_addr + deadline on every wake — wake-all spurious wakes from
// __cajeta_task_complete converge naturally (recheck rejects them) without
// needing a wake_reason CAS. The race to manage is "timer wake vs.
// concurrent wake-all": both take __cajeta_task_mutex and use parked-list
// membership as the gate, so the fiber is detached and published exactly
// once. If timer fires while the fiber isn't on parked_head (running, or
// already published), the timer entry is consumed (removed from the timer
// list) and the fiber catches the expired deadline via the now_ns check
// on its next park-loop iteration.
//
// Timer entries are owned by the FIBER'S STACK (one per __cajeta_task_wait_timeout
// call), so the timer thread never frees memory it didn't allocate. The fiber
// cancels its own entry before returning; cancel is idempotent against
// already-consumed entries via a linear walk of the live list.

#include <time.h>
#include <errno.h>

// Statics (head, cond, thread, started, shutdown_requested) declared
// above the carrier section so __cajeta_task_shutdown can join the
// timer thread on teardown; the struct definition follows here.
struct cajeta_timer_entry {
    int64_t deadline_ns;
    struct cajeta_fiber* fiber;
    struct cajeta_timer_entry* next;
};

static int64_t __cajeta_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + (int64_t) ts.tv_nsec;
}

// Insert entry into the sorted-by-deadline list. Caller holds __cajeta_task_mutex.
static void __cajeta_timer_insert_locked(struct cajeta_timer_entry* e) {
    struct cajeta_timer_entry** p = &__cajeta_timer_head;
    while (*p && (*p)->deadline_ns <= e->deadline_ns) p = &(*p)->next;
    e->next = *p;
    *p = e;
}

// Remove `f` from __cajeta_parked_head if present. Returns 1 if removed.
// Caller holds __cajeta_task_mutex.
static int __cajeta_parked_remove_locked(struct cajeta_fiber* f) {
    if (!__cajeta_parked_head) return 0;
    if (__cajeta_parked_head == f) {
        __cajeta_parked_head = f->next;
        f->next = NULL;
        return 1;
    }
    for (struct cajeta_fiber* p = __cajeta_parked_head; p->next; p = p->next) {
        if (p->next == f) {
            p->next = f->next;
            f->next = NULL;
            return 1;
        }
    }
    return 0;
}

// Timer thread. Sleeps on __cajeta_timer_cond until the next deadline (or
// a register/cancel/shutdown signal), wakes expired fibers, sleeps again.
static void* __cajeta_timer_loop(void* arg) {
    (void) arg;
    pthread_mutex_lock(&__cajeta_task_mutex);
    for (;;) {
        if (__cajeta_timer_shutdown_requested) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            return NULL;
        }
        int64_t now = __cajeta_now_ns();
        struct cajeta_fiber* to_publish = NULL;
        while (__cajeta_timer_head && __cajeta_timer_head->deadline_ns <= now) {
            struct cajeta_timer_entry* e = __cajeta_timer_head;
            __cajeta_timer_head = e->next;
            e->next = NULL;
            // Best-effort detach + publish. If fiber isn't on parked_head,
            // it's running (or already on a deque); the fiber's next park-
            // loop iteration will see the expired deadline.
            if (__cajeta_parked_remove_locked(e->fiber)) {
                e->fiber->next = to_publish;
                to_publish = e->fiber;
            }
        }
        if (to_publish) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            while (to_publish) {
                struct cajeta_fiber* next = to_publish->next;
                __cajeta_publish_ready(to_publish);
                to_publish = next;
            }
            pthread_mutex_lock(&__cajeta_task_mutex);
            continue;
        }
        if (__cajeta_timer_head) {
            int64_t deadline = __cajeta_timer_head->deadline_ns;
            struct timespec ts;
            ts.tv_sec = (time_t) (deadline / 1000000000LL);
            ts.tv_nsec = (long) (deadline % 1000000000LL);
            // CLOCK_REALTIME-based timedwait — the default. Deadline is
            // monotonic ns, which works regardless of clock choice for
            // an upper-bound sleep (the worst case is a stale clock
            // sleeping longer than needed; the fiber-side deadline check
            // catches up either way).
            pthread_cond_timedwait(&__cajeta_timer_cond,
                                    &__cajeta_task_mutex, &ts);
        } else {
            pthread_cond_wait(&__cajeta_timer_cond, &__cajeta_task_mutex);
        }
    }
}

// Lazy-start the timer thread on first registration. Caller holds task_mutex.
static void __cajeta_timer_ensure_started_locked(void) {
    if (__cajeta_timer_started) return;
    __cajeta_timer_started = 1;
    __cajeta_live_set_go_multithreaded();   // second-thread barrier (see live-set)
    pthread_create(&__cajeta_timer_thread, NULL, __cajeta_timer_loop, NULL);
}

// Cancel a timer entry. No-op if already consumed by the timer thread
// (not in the live list anymore). Caller must NOT hold task_mutex.
static void __cajeta_timer_cancel(struct cajeta_timer_entry* entry) {
    pthread_mutex_lock(&__cajeta_task_mutex);
    struct cajeta_timer_entry** p = &__cajeta_timer_head;
    while (*p) {
        if (*p == entry) {
            *p = entry->next;
            entry->next = NULL;
            break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// R9.1 intrinsic — block until *done_addr flips OR deadline_ns is reached.
// Returns 1 on done, 0 on timeout. Mirrors __cajeta_task_wait but adds a
// per-call timer entry that wakes the fiber at the deadline. deadline_ns
// is a CLOCK_MONOTONIC absolute timestamp (use __cajeta_currentTimeNanos
// to compute one from a duration).
int32_t __cajeta_task_wait_timeout(int32_t* done_addr, int64_t deadline_ns) {
    if (!done_addr) return 1;
    if (!__cajeta_current_fiber) {
        // Main thread / non-fiber caller: cond_timedwait on the existing
        // task-done condvar. The condvar's clock is CLOCK_REALTIME; convert
        // our monotonic deadline by computing the remaining nanos and
        // adding to a fresh REALTIME `now`. Sufficient for the timeout
        // ceiling; precision-critical use should switch to a CLOCK_MONOTONIC
        // condvar (pthread_condattr_setclock) when a user surfaces a need.
        pthread_mutex_lock(&__cajeta_task_mutex);
        while (!*done_addr) {
            int64_t mono_now = __cajeta_now_ns();
            if (mono_now >= deadline_ns) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                return 0;
            }
            int64_t remaining_ns = deadline_ns - mono_now;
            struct timespec real_ts;
            clock_gettime(CLOCK_REALTIME, &real_ts);
            int64_t real_deadline = (int64_t) real_ts.tv_sec * 1000000000LL
                                  + (int64_t) real_ts.tv_nsec
                                  + remaining_ns;
            real_ts.tv_sec = (time_t) (real_deadline / 1000000000LL);
            real_ts.tv_nsec = (long) (real_deadline % 1000000000LL);
            int rc = pthread_cond_timedwait(&__cajeta_task_done_cond,
                                             &__cajeta_task_mutex,
                                             &real_ts);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                return 0;
            }
        }
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return 1;
    }
    // Fiber path. Stack-local entry: lifetime = this function, which is
    // safe because the fiber's stack persists across park/resume.
    struct cajeta_timer_entry entry;
    entry.deadline_ns = deadline_ns;
    entry.fiber = __cajeta_current_fiber;
    entry.next = NULL;
    pthread_mutex_lock(&__cajeta_task_mutex);
    __cajeta_timer_ensure_started_locked();
    int signal_timer = (__cajeta_timer_head == NULL
                        || __cajeta_timer_head->deadline_ns > deadline_ns);
    __cajeta_timer_insert_locked(&entry);
    pthread_mutex_unlock(&__cajeta_task_mutex);
    if (signal_timer) {
        pthread_cond_signal(&__cajeta_timer_cond);
    }
    for (;;) {
        // Recheck both wake conditions (task done / deadline passed) under
        // the mutex that task_complete and the timer thread use, then park
        // atomically — same lost-wakeup fix as __cajeta_task_wait. The timer
        // thread, if it fires before we park, can't find us on parked_head
        // and consumes our entry; we observe the elapsed deadline here and
        // return 0 instead of parking with no waker left.
        pthread_mutex_lock(&__cajeta_task_mutex);
        if (*done_addr) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_timer_cancel(&entry);
            return 1;
        }
        if (__cajeta_now_ns() >= deadline_ns) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_timer_cancel(&entry);
            return 0;
        }
        __cajeta_fiber_park_locked();  // releases the mutex, then swaps
        // Mirror __cajeta_task_wait's R5-C cancel handling: a scope's
        // first-throw escalation may have set cancel_with while we were
        // parked; honor it before looping (we still cancel our timer so
        // the entry doesn't outlive this fiber's stack frame).
        void* cancel = __cajeta_current_fiber->cancel_with;
        if (cancel) {
            __cajeta_current_fiber->cancel_with = NULL;
            __cajeta_timer_cancel(&entry);
            __cajeta_throw(cancel);
        }
    }
}

// R9.1 intrinsic — current CLOCK_MONOTONIC nanoseconds. Surfaced for stdlib
// computing a deadline from a Duration (R9.3): `now() + d.toNanos()`.
int64_t __cajeta_currentTimeNanos(void) {
    return __cajeta_now_ns();
}

// R9.5 — fiber-aware sleep. Park the running fiber on the timer wheel for
// up to `nanos` nanoseconds. Built on top of __cajeta_task_wait_timeout
// by feeding it a sentinel done_addr that never flips — the wait then
// always resolves via the deadline. From the main thread / non-fiber
// caller, cond_timedwait on the same condvar (same fallback the timeout
// path takes). Used by Channel.select's polling backoff; available to
// other callers wanting a cooperative sleep.
void __cajeta_fiber_sleep_nanos(int64_t nanos) {
    if (nanos <= 0) return;
    int32_t never = 0;
    int64_t deadline = __cajeta_now_ns() + nanos;
    (void) __cajeta_task_wait_timeout(&never, deadline);
}

// --- R9.4 — I/O reactor / netpoller -----------------------------------------
//
// Goal: park a fiber on file-descriptor readiness without freezing the
// carrier OS thread. The intrinsic __cajeta_io_wait(fd, events_mask) blocks
// the calling fiber until any of the requested events fires on `fd`, then
// returns 1; non-fiber callers fall through to a direct blocking
// epoll_wait so the surface API is uniform across both contexts.
//
// Mechanism: a single epoll fd owned by a dedicated reactor thread.
// __cajeta_io_wait registers (fd, requested events, current fiber) with
// the reactor and parks. The reactor's epoll_wait loop wakes, finds the
// matching waiter(s), detaches each fiber from __cajeta_parked_head, and
// republishes via __cajeta_publish_ready. EPOLLONESHOT ensures each fd
// auto-cleans from epoll after firing; the per-call waiter struct lives
// on the heap (single per fiber, multiple fibers may wait on different
// fds) and is freed by the wake path.
//
// v1 limitations:
//   - Linux only. macOS (kqueue) and Windows (IOCP) are stubbed; the
//     intrinsic returns -1 there.
//   - Each io_wait call sets up its own epoll registration; long-lived
//     persistent registrations (the standard netpoller pattern for high
//     fd counts) come when a real consumer surfaces.
//   - No deadline parameter v1. Deadlines compose with the R9.1 timer —
//     a withIoTimeout(d, fd, events) helper at the cajeta level can layer
//     both. Deferred until a use case lands.
//   - One waiter per fd in v1. Two fibers waiting on the same fd would
//     have only one notified (whichever the epoll_ctl_add saw second
//     would EEXIST and we degrade to MOD). Real netpoller semantics
//     (separate read/write waiter queues per fd) lands later.

#if defined(__linux__)
#  include <sys/epoll.h>
#  include <sys/eventfd.h>
#endif

#define CAJETA_IO_READ  1
#define CAJETA_IO_WRITE 2

struct cajeta_io_waiter {
    int fd;
    int events;
    struct cajeta_fiber* fiber;
    struct cajeta_io_waiter* next;
    // Timed-wait support (__cajeta_io_wait_timed). A timed waiter lives on
    // the WAITING FIBER'S STACK (safe: the stack persists across park/
    // resume — the cajeta_timer_entry ownership rule), so the reactor wake
    // path must not free it: it marks `fired` instead, and the fiber reads
    // that flag under task_mutex to tell an I/O wake from a deadline wake.
    // Untimed (heap) waiters keep the original reactor-frees-on-wake
    // contract with both fields zero.
    int stack_owned;
    int fired;
};

#if defined(__linux__)

static int __cajeta_io_events_to_epoll(int events) {
    int e = 0;
    if (events & CAJETA_IO_READ)  e |= EPOLLIN;
    if (events & CAJETA_IO_WRITE) e |= EPOLLOUT;
    return e | EPOLLONESHOT;
}

// Reactor thread main loop. Polls epoll_wait with a 1-second timeout so
// the shutdown flag is observed even when no I/O is in flight. On each
// ready event, walks the waiter list under task_mutex, detaches matched
// fibers from __cajeta_parked_head, and publishes them.
static void* __cajeta_reactor_loop(void* arg) {
    (void) arg;
    for (;;) {
        if (__cajeta_reactor_shutdown_requested) return NULL;
        struct epoll_event ep[64];
        int n = epoll_wait(__cajeta_reactor_epfd, ep, 64, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        if (n == 0) continue;
        struct cajeta_fiber* to_publish = NULL;
        pthread_mutex_lock(&__cajeta_task_mutex);
        for (int i = 0; i < n; ++i) {
            int fd = ep[i].data.fd;
            struct cajeta_io_waiter** p = &__cajeta_reactor_waiters;
            while (*p) {
                if ((*p)->fd == fd) {
                    struct cajeta_io_waiter* w = *p;
                    *p = w->next;
                    // EPOLLONESHOT disarmed the fd's registration (it stays
                    // in the interest list, disabled, until DEL or close —
                    // the ADD→EEXIST→MOD path in io_wait re-arms it); just
                    // detach + publish the fiber.
                    w->fired = 1;
                    if (__cajeta_parked_remove_locked(w->fiber)) {
                        w->fiber->next = to_publish;
                        to_publish = w->fiber;
                    }
                    // A stack-owned waiter (timed wait) belongs to the
                    // fiber's frame — the fiber reads `fired` after resume.
                    if (!w->stack_owned) free(w);
                } else {
                    p = &(*p)->next;
                }
            }
        }
        pthread_mutex_unlock(&__cajeta_task_mutex);
        while (to_publish) {
            struct cajeta_fiber* next = to_publish->next;
            __cajeta_publish_ready(to_publish);
            to_publish = next;
        }
    }
}

// Lazy-start the reactor on first registration. Caller holds task_mutex.
static void __cajeta_reactor_ensure_started_locked(void) {
    if (__cajeta_reactor_started) return;
    __cajeta_reactor_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (__cajeta_reactor_epfd < 0) {
        fprintf(stderr, "cajeta: epoll_create1 failed: %d\n", errno);
        return;
    }
    __cajeta_reactor_started = 1;
    __cajeta_live_set_go_multithreaded();   // second-thread barrier (see live-set)
    pthread_create(&__cajeta_reactor_thread, NULL,
                    __cajeta_reactor_loop, NULL);
}

// Cancel a waiter (clear the registration) on the error/cleanup path.
// Caller holds task_mutex; entry must NOT have been published yet.
static void __cajeta_reactor_cancel_locked(struct cajeta_io_waiter* w) {
    struct cajeta_io_waiter** p = &__cajeta_reactor_waiters;
    while (*p) {
        if (*p == w) { *p = w->next; break; }
        p = &(*p)->next;
    }
    epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_DEL, w->fd, NULL);
}

int32_t __cajeta_io_wait(int32_t fd, int32_t events) {
    if (!__cajeta_current_fiber) {
        // Non-fiber caller: skip the reactor and just do a direct
        // blocking epoll_wait. Same observable surface — 1 on ready,
        // 0/-1 on error — without paying for reactor lazy-start.
        int epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0) return -1;
        struct epoll_event ep;
        ep.events = __cajeta_io_events_to_epoll(events);
        ep.data.fd = fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ep) < 0) {
            close(epfd);
            return -1;
        }
        struct epoll_event got;
        int n;
        do {
            n = epoll_wait(epfd, &got, 1, -1);
        } while (n < 0 && errno == EINTR);
        close(epfd);
        return (n > 0) ? 1 : 0;
    }
    pthread_mutex_lock(&__cajeta_task_mutex);
    __cajeta_reactor_ensure_started_locked();
    if (!__cajeta_reactor_started) {
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return -1;
    }
    struct cajeta_io_waiter* w = malloc(sizeof(*w));
    if (!w) {
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return -1;
    }
    w->fd = fd;
    w->events = events;
    w->fiber = __cajeta_current_fiber;
    w->stack_owned = 0;
    w->fired = 0;
    w->next = __cajeta_reactor_waiters;
    __cajeta_reactor_waiters = w;
    struct epoll_event ep;
    ep.events = __cajeta_io_events_to_epoll(events);
    ep.data.fd = fd;
    int rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_ADD, fd, &ep);
    if (rc < 0 && errno == EEXIST) {
        rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_MOD, fd, &ep);
    }
    if (rc < 0) {
        __cajeta_reactor_cancel_locked(w);
        free(w);
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return -1;
    }
    // Park while STILL holding task_mutex (park_locked releases it). The
    // waiter was registered under this same lock, so by the time the mutex
    // is dropped the fiber is already on parked_head — the reactor thread,
    // which needs task_mutex to match + wake waiters, therefore cannot fire
    // and free our waiter in the gap before we park (the EPOLLONESHOT event
    // is one-shot, so a missed wake would hang the fiber permanently).
    __cajeta_fiber_park_locked();
    return 1;
}

// Deadline-bounded fiber I/O wait — the fiber-parking twin of the blocking
// __cajeta_net_reactor_poll_fd probe. Returns 1 (ready), 0 (deadline
// elapsed first), or -1 (setup error).
//
// WHY THIS EXISTS (the carrier-starvation bug): Reactor.awaitReadableTimed
// used to run the portable select() probe, which blocks the calling OS
// THREAD. On a fiber that means the whole carrier stalls for up to the
// deadline — and every fiber co-hosted on that carrier starves with it.
// cajeta-http's server head-read (readWithin, 30s budget) parked its
// carrier while the CLIENT fiber that would have sent the request bytes
// sat un-runnable on the same carrier's deque: the head read then "timed
// out" against a peer that was never allowed to run, the server dropped
// the connection without a response, and the client saw EOF mid-head —
// a scheduling-roulette flake (~25% per run, layout-sensitive) that
// reproduced as a failing run taking exactly the 30s head budget while
// passing runs took 6ms.
//
// Mechanism: combine the reactor's one-shot fd waiter with the R9.1 timer
// wheel — both entries live on THIS FIBER'S STACK (the timer-entry
// ownership rule; the stack persists across park/resume), both armed
// under the one task_mutex, and the park loop re-checks both wake
// conditions under that same mutex so the reactor-thread wake, the
// timer-thread wake, and a scope cancellation can each fire exactly once
// with no lost-wakeup window:
//   - reactor wake: detaches the waiter, sets w.fired (stack-owned, so it
//     does NOT free), publishes the fiber → loop sees fired → 1.
//   - timer wake: consumes the timer entry, publishes the fiber → loop
//     sees the elapsed deadline → cancels the waiter (detach + epoll DEL)
//     → 0. The DEL also clears the EPOLLONESHOT registration the wake
//     path would otherwise have left disarmed-but-registered.
//   - both race: fired wins (the data IS there); the loser's entry is
//     cancelled idempotently.
int32_t __cajeta_io_wait_timed(int32_t fd, int32_t events, int32_t timeout_ms) {
    if (timeout_ms < 0) {
        // Unbounded: the plain park path already has the right semantics.
        return __cajeta_io_wait(fd, events);
    }
    if (!__cajeta_current_fiber) {
        // Non-fiber caller (the main thread): a throwaway epoll with the
        // deadline — blocking the caller is the correct semantic here.
        int epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0) return -1;
        struct epoll_event ep;
        ep.events = __cajeta_io_events_to_epoll(events);
        ep.data.fd = fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ep) < 0) {
            close(epfd);
            return -1;
        }
        struct epoll_event got;
        int n;
        do {
            n = epoll_wait(epfd, &got, 1, timeout_ms);
        } while (n < 0 && errno == EINTR);
        close(epfd);
        if (n < 0) return -1;
        return (n > 0) ? 1 : 0;
    }

    int64_t deadline_ns = __cajeta_now_ns()
                        + (int64_t) timeout_ms * 1000000LL;
    struct cajeta_io_waiter w;
    w.fd = fd;
    w.events = events;
    w.fiber = __cajeta_current_fiber;
    w.stack_owned = 1;
    w.fired = 0;
    struct cajeta_timer_entry entry;
    entry.deadline_ns = deadline_ns;
    entry.fiber = __cajeta_current_fiber;
    entry.next = NULL;

    pthread_mutex_lock(&__cajeta_task_mutex);
    __cajeta_reactor_ensure_started_locked();
    if (!__cajeta_reactor_started) {
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return -1;
    }
    w.next = __cajeta_reactor_waiters;
    __cajeta_reactor_waiters = &w;
    struct epoll_event ep;
    ep.events = __cajeta_io_events_to_epoll(events);
    ep.data.fd = fd;
    int rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_ADD, fd, &ep);
    if (rc < 0 && errno == EEXIST) {
        rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_MOD, fd, &ep);
    }
    if (rc < 0) {
        __cajeta_reactor_cancel_locked(&w);
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return -1;
    }
    __cajeta_timer_ensure_started_locked();
    int signal_timer = (__cajeta_timer_head == NULL
                        || __cajeta_timer_head->deadline_ns > deadline_ns);
    __cajeta_timer_insert_locked(&entry);
    pthread_mutex_unlock(&__cajeta_task_mutex);
    if (signal_timer) {
        pthread_cond_signal(&__cajeta_timer_cond);
    }

    for (;;) {
        // Re-check both wake conditions under the mutex the reactor and
        // timer threads use, then park atomically — the same lost-wakeup
        // bracket as __cajeta_task_wait_timeout.
        pthread_mutex_lock(&__cajeta_task_mutex);
        if (w.fired) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_timer_cancel(&entry);
            return 1;
        }
        if (__cajeta_now_ns() >= deadline_ns) {
            // Deadline first: withdraw the (stack-owned) waiter so the
            // reactor can never touch this frame after we return, and
            // clear the epoll registration.
            __cajeta_reactor_cancel_locked(&w);
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_timer_cancel(&entry);
            return 0;
        }
        __cajeta_fiber_park_locked();  // releases the mutex, then swaps
        // R5-C: honor a scope cancellation delivered while parked —
        // withdraw BOTH entries first (they are stack memory about to
        // unwind with the throw).
        void* cancel = __cajeta_current_fiber->cancel_with;
        if (cancel) {
            __cajeta_current_fiber->cancel_with = NULL;
            pthread_mutex_lock(&__cajeta_task_mutex);
            __cajeta_reactor_cancel_locked(&w);
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_timer_cancel(&entry);
            __cajeta_throw(cancel);
        }
    }
}

// Linux eventfd surface, exposed for test bring-up and for cooperative
// cross-fiber signalling. eventfd is a counter the kernel guarantees is
// edge-sensitive on writes — perfect for the "one-shot ready" pattern
// the I/O reactor needs to verify end-to-end.
int32_t __cajeta_eventfd_create(void) {
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    return (fd < 0) ? -1 : (int32_t) fd;
}

int32_t __cajeta_eventfd_signal(int32_t fd) {
    uint64_t one = 1;
    ssize_t n = write(fd, &one, sizeof(one));
    return (n == (ssize_t) sizeof(one)) ? 0 : -1;
}

int64_t __cajeta_eventfd_consume(int32_t fd) {
    uint64_t buf = 0;
    ssize_t n = read(fd, &buf, sizeof(buf));
    return (n == (ssize_t) sizeof(buf)) ? (int64_t) buf : -1;
}

int32_t __cajeta_fd_close(int32_t fd) {
    return close(fd);
}

#else /* !__linux__ */

int32_t __cajeta_io_wait(int32_t fd, int32_t events) {
    (void) fd; (void) events;
    fprintf(stderr, "cajeta: __cajeta_io_wait not yet implemented on this platform\n");
    return -1;
}

int32_t __cajeta_io_wait_timed(int32_t fd, int32_t events, int32_t timeout_ms) {
    (void) fd; (void) events; (void) timeout_ms;
    fprintf(stderr,
        "cajeta: __cajeta_io_wait_timed not yet implemented on this platform\n");
    return -1;
}
int32_t __cajeta_eventfd_create(void) {
    fprintf(stderr, "cajeta: __cajeta_eventfd_create requires Linux\n");
    return -1;
}
int32_t __cajeta_eventfd_signal(int32_t fd) { (void) fd; return -1; }
int64_t __cajeta_eventfd_consume(int32_t fd) { (void) fd; return -1; }
int32_t __cajeta_fd_close(int32_t fd) { (void) fd; return -1; }

#endif /* __linux__ */

// --- Threading: scope frames (R5-A) ---------------------------------------
