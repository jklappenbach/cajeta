// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- Threading sync primitives: Lock --------------------------------------

// The lock primitives live AFTER the fiber executor so they can use the fiber
// struct and carrier state directly; shared infrastructure comes first.

// --- Threading: stackful fiber executor (R3-B) ----------------------------
// Each spawn produces a fiber run cooperatively on a carrier OS thread via
// ucontext. The main thread is NOT a fiber and OS-blocks on a condvar.

// MinGW-w64 ships no ucontext.h, so Windows gets a shim over the Win32 Fibers
// API; its ucontext_t keeps the uc_stack/uc_link fields the fiber init reads.
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>   // BCryptGenRandom for cajeta_fill_entropy; must be at
                        // file scope, and mingw ignores `#pragma comment(lib)`
                        // — the import lib is linked from src/CMakeLists.txt.

typedef struct {
    LPVOID fiber;                         // Windows fiber handle
    void (*entry)(void);                  // makecontext-supplied entry
    struct { void* ss_sp; size_t ss_size; } uc_stack;
    void* uc_link;
} ucontext_t;

// Win32 fiber entry: runs the makecontext-supplied entry, then hands control to
// uc_link — NOT GetCurrentFiber(): SwitchToFiber(self) is undefined, and lets
// Windows terminate the carrier thread once the first task finishes.
static VOID CALLBACK __cajeta_w32_fiber_trampoline(LPVOID param) {
    ucontext_t* uc = (ucontext_t*) param;
    if (uc && uc->entry) uc->entry();
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

// Apple deprecated ucontext.h with no replacement for user-space context
// switching; the wrappers silence the deprecation in exactly one place.
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

// Per-fiber stack size. A fiber calls into native libraries (an OpenSSL
// handshake wants well over 64 KB); overridable via $CAJETA_FIBER_STACK_KB.
#define CAJETA_FIBER_STACK_SIZE (1024 * 1024)

// Resolves the stack size once, honoring $CAJETA_FIBER_STACK_KB (KiB) when it
// is at least 64. Cached, so every fiber in the process gets the same size.
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

// Page-rounded allocation size, so the free path can reconstruct the exact
// mapping from the cached size alone.
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

// Allocates one fiber stack. POSIX maps guard-page + stack with the LOWEST page
// PROT_NONE (stacks grow down), so an overflow faults at the overflowing frame;
// Windows uses malloc, since CreateFiber allocates its own. Aborts on failure.
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

// Frees a stack from __cajeta_fiber_stack_alloc, including its guard page.
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

// Forward decl — scope_exit re-raises through __cajeta_throw, defined later.
__attribute__((noreturn)) void __cajeta_throw(void* value);

// Per-scope tracking of spawned child tasks: the closing `}` waits on every
// registered done flag, then re-raises the first exception slot it finds.
struct cajeta_scope_entry {
    int32_t* done_addr;
    void** exception_addr;  // points to the Throwable* slot; NULL on success
    void** fiber_slot;      // points to Task's fiber-ptr slot; runtime fills it
    // Non-NULL iff the SCOPE owns freeing this task struct (a DISCARDED
    // `spawn`, which no local binds); NULL when a bound Task's drop frees it.
    void* owned_task;
};

struct cajeta_scope_frame {
    struct cajeta_scope_entry* entries;
    int count;
    int cap;
    struct cajeta_scope_frame* prev;
};

// Forward decls: both chains are defined further down in this file.
struct cajeta_drop_entry;
struct cajeta_exception_frame;

// FiberLocal binding frame; full definition in the § FiberLocal section below.
struct cajeta_fiber_local;

struct cajeta_fiber {
    ucontext_t ctx;
    void* stack;
    cajeta_fiber_state state;
    cajeta_task_trampoline_fn trampoline;
    void* trampoline_arg;
    struct cajeta_fiber* next;
    // Per-fiber scope chain — a __thread slot would alias across fiber switches.
    struct cajeta_scope_frame* scope_top;
    // Per-fiber drop and exception chain heads, same aliasing rationale;
    // __cajeta_drop_top_ptr / __cajeta_exc_top_ptr pick the fiber or main slot.
    struct cajeta_drop_entry* drop_top;
    struct cajeta_exception_frame* exc_top;
    // FiberLocal binding stack; a fresh fiber inherits a deep copy (task_run).
    struct cajeta_fiber_local* fl_top;
    // Cancellation marker: when non-NULL the fiber's next task_wait resume
    // throws this Throwable* instead of returning.
    void* cancel_with;
    // Address of the Task's fiber-ptr slot. The carrier nulls it under
    // __cajeta_task_mutex before freeing, so a concurrent cancel never derefs it.
    void** slot_ptr;
    // Stable per-fiber debug id (fibers get 1,2,3...; the main thread is 0).
    int dbg_id;
    // Per-fiber debug frame-chain head, selected by __cajeta_dbg_top_ptr.
    struct cajeta_dbg_frame* dbg_top;
    // Per-fiber line-info shadow stack. Inline, not a pointer, to keep the
    // enter/mark/leave path allocation-free; selected by __cajeta_shadow_ptr.
    CajetaShadowStack shadow;
    // Per-fiber frame arena — the LIFO mark/reset discipline holds per logical
    // stack, so a shared one let one fiber's reset free a parked fiber's memory.
    cajeta_arena arena;
    // Home carrier: the one that FIRST dispatched this fiber, -1 until then. A
    // started fiber's saved ucontext is bound to that carrier, so only it may
    // resume; fresh fibers have no saved context and may run anywhere.
    int home_carrier;
};

static pthread_mutex_t __cajeta_task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __cajeta_task_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  __cajeta_task_done_cond  = PTHREAD_COND_INITIALIZER;

// FiberLocal helpers, defined in the § FiberLocal section further down.
static struct cajeta_fiber_local* __cajeta_fiber_local_snapshot_current(void);
static void __cajeta_fiber_local_free_chain(struct cajeta_fiber_local* head);

// Per-carrier Chase-Lev work-stealing deque: single-producer at `bottom` (the
// owning carrier pushes/pops LIFO), multi-consumer at `top` (peers steal FIFO).
// Fixed circular slots indexed `seq % CAJETA_DEQUE_CAP`; overflow aborts.
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

// Owner-side pop (LIFO end). NULL on empty. The Chase-Lev "last element" CAS
// is what keeps the owner / stealer race tight down to one fiber.
static struct cajeta_fiber* __cajeta_deque_pop_bottom(
        struct cajeta_carrier_deque* d) {
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_RELAXED) - 1;
    __atomic_store_n(&d->bottom, b, __ATOMIC_RELAXED);
    // The SeqCst fence pairs with steal()'s SeqCst fence; without it,
    // pop_bottom and steal can both think they got the last element.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_RELAXED);
    if (t > b) {
        __atomic_store_n(&d->bottom, t, __ATOMIC_RELAXED);
        return NULL;
    }
    struct cajeta_fiber* f = d->slots[b % CAJETA_DEQUE_CAP];
    if (t < b) {
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

// Stealer-side pop (FIFO end). NULL on empty, or on a CAS race lost to the
// owner or another stealer.
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

// Snapshot of the deque's size from the owner's side. Callers hold
// __cajeta_task_mutex, so the loads can be relaxed.
static int64_t __cajeta_deque_size(struct cajeta_carrier_deque* d) {
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_RELAXED);
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_RELAXED);
    return b - t;
}
// Parked list: fibers blocked in __cajeta_task_wait. Wake-all-on-any-complete,
// protected by __cajeta_task_mutex.
static struct cajeta_fiber* __cajeta_parked_head = NULL;
static int __cajeta_task_workers_started = 0;
// Set by __cajeta_task_shutdown; the carrier's wait predicate checks it.
static int __cajeta_task_shutdown_requested = 0;

// R9.1 timer state, declared here so __cajeta_task_shutdown — which sits above
// the timer implementation — can join the timer thread on teardown.
struct cajeta_timer_entry;
static struct cajeta_timer_entry* __cajeta_timer_head = NULL;
static pthread_cond_t __cajeta_timer_cond = PTHREAD_COND_INITIALIZER;
static pthread_t __cajeta_timer_thread;
static int __cajeta_timer_started = 0;
static int __cajeta_timer_shutdown_requested = 0;

// R9.4 reactor state, declared here for the same reason as the timer state.
struct cajeta_io_waiter;
static struct cajeta_io_waiter* __cajeta_reactor_waiters = NULL;
static pthread_t __cajeta_reactor_thread;
static int __cajeta_reactor_started = 0;
static int __cajeta_reactor_shutdown_requested = 0;
#if defined(__linux__)
static int __cajeta_reactor_epfd = -1;
#endif

// Net-reactor teardown hook (cajeta_net_reactor_lifecycle.c, #included at the
// bottom of this TU). Idempotent, and a no-op when no awaitable net op ran.
int32_t __cajeta_net_reactor_shutdown(void);

// Multi-carrier pool. Each carrier owns a deque plus a deque_mutex for the
// owner-side ops; steals stay lock-free, pool coordination takes task_mutex.
#define CAJETA_MAX_CARRIERS 16
// Default cap, so a many-core box spins up no carriers it has no work for.
#define CAJETA_DEFAULT_CARRIERS_CAP 4

struct cajeta_carrier {
    pthread_t thread;
    int carrier_id;
    pthread_mutex_t deque_mutex;
    struct cajeta_carrier_deque deque;
};

static struct cajeta_carrier __cajeta_carriers[CAJETA_MAX_CARRIERS];
static int __cajeta_carrier_count = 0;
// Carriers currently parked in cond_wait; pushers signal only when non-zero.
static int __cajeta_sleeping_count = 0;

// The carrier running on this thread, NULL elsewhere (routes work for locality).
static __thread struct cajeta_carrier* __cajeta_my_carrier = NULL;

// The fiber running on this OS thread; NULL on the main thread.
static __thread struct cajeta_fiber* __cajeta_current_fiber = NULL;

// Frame-arena selector: the running fiber's own arena, else the thread's.
static cajeta_arena* __cajeta_arena_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->arena;
    }
    return &__cajeta_arena;
}
static __thread ucontext_t __cajeta_carrier_ctx;

// Scope chain for code running outside a fiber (main entry, non-carrier thread).
static __thread struct cajeta_scope_frame* __cajeta_main_scope_top = NULL;

// Main/program-thread slot for the debugger's frame chain.
static __thread struct cajeta_dbg_frame* __cajeta_main_dbg_top = NULL;

// Returns the live scope_top slot — the running fiber's, or the main thread's
// TLS — so push/pop works uniformly in either context.
static struct cajeta_scope_frame** __cajeta_scope_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->scope_top;
    }
    return &__cajeta_main_scope_top;
}

// Selector for the debug frame-chain head, mirroring __cajeta_scope_top_ptr.
struct cajeta_dbg_frame** __cajeta_dbg_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->dbg_top;
    }
    return &__cajeta_main_dbg_top;
}

// Selector for the live line-info shadow stack.
CajetaShadowStack* __cajeta_shadow_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->shadow;
    }
    return &__cajeta_main_shadow;
}

// Forward decl; defined below, where __cajeta_current_fiber is in scope.
int __cajeta_dbg_on_program_thread(void);

// Debug id of the fiber on this carrier thread, 0 on the program thread, -1 for
// a carrier outside fiber context (so it can't satisfy a step armed on fiber 0).
int __cajeta_dbg_current_fiber_id(void) {
    if (__cajeta_current_fiber) return __cajeta_current_fiber->dbg_id;
    return __cajeta_dbg_on_program_thread() ? 0 : -1;
}

// Stateless per-fiber accessors. They cast an opaque handle (from
// __cajeta_dbg_fiber_at) back to the fiber struct, so the host's NATIVE runtime
// copy can read a fiber the JIT copy registered (identical struct layout).
long __cajeta_dbg_fiber_id_of(void* fiber) {
    return fiber ? (long) ((struct cajeta_fiber*) fiber)->dbg_id : 0;
}

// The fiber's shadow stack, for the sampler. It sits well inside struct
// cajeta_fiber, behind a ucontext_t — a fiber handle is NOT a shadow stack.
void* __cajeta_dbg_fiber_shadow_of(void* fiber) {
    return fiber ? (void*) &((struct cajeta_fiber*) fiber)->shadow : NULL;
}

void* __cajeta_dbg_fiber_frame_top(void* fiber) {
    return fiber ? ((struct cajeta_fiber*) fiber)->dbg_top : NULL;
}

int __cajeta_dbg_fiber_state(void* fiber) {
    return fiber ? (int) ((struct cajeta_fiber*) fiber)->state : -1;
}

// Fiber entry trampoline, invoked by makecontext on first resume. Swaps back
// explicitly to the RUNNING carrier's TLS slot rather than through uc_link,
// whose baked-in address names the carrier that first dispatched the fiber.
static void __cajeta_fiber_entry(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    f->trampoline(f->trampoline_arg);
    f->state = CAJETA_FIBER_DONE;
    __cajeta_swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// Publishes a ready fiber onto a carrier's deque — the current carrier's when
// the caller runs on one, else carrier 0 — and signals a sleeper. The caller
// must NOT hold __cajeta_task_mutex; this takes it for the signal step.
static void __cajeta_publish_ready(struct cajeta_fiber* f) {
    f->state = CAJETA_FIBER_READY;
    f->next = NULL;
    // A started fiber is pinned to its home carrier: its saved ucontext can only
    // be resumed there. A fresh one goes to the waker's carrier for locality.
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

// Detaches every parked fiber and returns them as a NULL-terminated list.
// Caller holds __cajeta_task_mutex and must publish them AFTER releasing it —
// publishing under the pool mutex inverts publish_ready's deque → pool order.
static struct cajeta_fiber* __cajeta_drain_parked_locked(void) {
    struct cajeta_fiber* drained = __cajeta_parked_head;
    __cajeta_parked_head = NULL;
    return drained;
}

// Total work across every carrier's deque, read lock-free. Carriers consult it
// before sleeping, so none waits while work is still reachable in the pool.
static int64_t __cajeta_pool_total_work(void) {
    int64_t total = 0;
    for (int i = 0; i < __cajeta_carrier_count; ++i) {
        total += __cajeta_deque_size(&__cajeta_carriers[i].deque);
    }
    return total;
}

// Steals a ready fiber from a peer's deque top, round-robin after `self`. NULL
// when every peer is empty or every attempt loses the CAS race.
static struct cajeta_fiber* __cajeta_steal_one(struct cajeta_carrier* self) {
    int n = __cajeta_carrier_count;
    if (n <= 1) return NULL;
    int start = self ? self->carrier_id + 1 : 0;
    for (int step = 0; step < n; ++step) {
        int idx = (start + step) % n;
        if (&__cajeta_carriers[idx] == self) continue;
        struct cajeta_fiber* f = __cajeta_deque_steal(&__cajeta_carriers[idx].deque);
        if (!f) continue;
        // Only a fresh fiber, or one already homed to us, may run here: a
        // started fiber pinned elsewhere has a ucontext bound to that carrier.
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

// Parks the running fiber: enqueues it on parked_head and swaps back to the
// carrier. The swap returns here only once a wake re-dispatches the fiber.
static void __cajeta_fiber_park(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    pthread_mutex_lock(&__cajeta_task_mutex);
    f->state = CAJETA_FIBER_PARKED;
    f->next = __cajeta_parked_head;
    __cajeta_parked_head = f;
    pthread_mutex_unlock(&__cajeta_task_mutex);
    __cajeta_swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// Park variant for callers that ALREADY hold __cajeta_task_mutex: enqueues on
// parked_head and releases the mutex before swapping, so no waker can fire
// between the caller's condition re-check and the enqueue (lost wakeup).
static void __cajeta_fiber_park_locked(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    f->state = CAJETA_FIBER_PARKED;
    f->next = __cajeta_parked_head;
    __cajeta_parked_head = f;
    pthread_mutex_unlock(&__cajeta_task_mutex);
    __cajeta_swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// Carrier loop: pop own deque (LIFO), else steal from a peer, else sleep on the
// pool condvar. The wrapper registers this thread's shadow stack.
static void* __cajeta_carrier_loop_body(void* arg);
static void* __cajeta_carrier_loop(void* arg) {
    __cajeta_prof_thread_register();
    void* r = __cajeta_carrier_loop_body(arg);
    __cajeta_prof_thread_unregister();
    return r;
}
static void* __cajeta_carrier_loop_body(void* arg) {
    struct cajeta_carrier* self = (struct cajeta_carrier*) arg;
    __cajeta_my_carrier = self;
    for (;;) {
        pthread_mutex_lock(&self->deque_mutex);
        struct cajeta_fiber* f = __cajeta_deque_pop_bottom(&self->deque);
        pthread_mutex_unlock(&self->deque_mutex);

        if (!f) {
            f = __cajeta_steal_one(self);
        }

        if (!f) {
            // Decide exit / retry / wait off a SINGLE pool_total_work() read:
            // two reads let a carrier that had already seen shutdown fall into
            // the one-shot condvar and never wake. The wait is a bounded backstop.
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
            // An idle carrier under a debugger stop parks instead — outside the
            // task mutex, since stop_mu is a leaf — so the barrier converges.
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

        if (__cajeta_stop_is_requested()) __cajeta_stop_park();

        __cajeta_current_fiber = f;
        f->state = CAJETA_FIBER_RUNNING;
        if (!f->stack) {
            // First-resume init: allocate the stack and prime the context to
            // dispatch __cajeta_fiber_entry. uc_link points at this thread's
            // carrier_ctx; the cross-carrier uc_link handoff is unsolved.
            size_t stack_size = __cajeta_fiber_stack_alloc_size();
            f->stack = __cajeta_fiber_stack_alloc();   // guard-paged on POSIX
            // First dispatch pins the fiber to this carrier (see home_carrier).
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
            // Null the Task's fiber slot before freeing, under the task mutex,
            // so a concurrent scope-cancel sees NULL, never a dangling fiber.
            pthread_mutex_lock(&__cajeta_task_mutex);
            if (f->slot_ptr) *f->slot_ptr = NULL;
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_dbg_fiber_unregister(f);
#if defined(_WIN32)
            // free() reclaims the struct but not the Win32 fiber object; delete
            // it here (it has returned to the carrier, so it isn't current).
            if (f->ctx.fiber) DeleteFiber(f->ctx.fiber);
#endif
            __cajeta_fiber_local_free_chain(f->fl_top);
            f->fl_top = NULL;
            __cajeta_arena_release_mapping(&f->arena);
            __cajeta_fiber_stack_free(f->stack);
            free(f);
        }
    }
    return NULL;
}

// Number of carrier threads in the current pool (0 if not started). The
// debugger's quiesce barrier uses it to size the expected park count.
int __cajeta_carrier_count_get(void) {
    return __cajeta_carrier_count;
}

// Defined in cajeta_xpu_dispatch.c (included after this file in the
// single-TU build): joins the persistent CPU-kernel worker pool.
void __cajeta_xpu_kpool_shutdown(void);

// Signals the carriers, timer and reactor to exit, joins them, and resets pool
// state. JIT-mode teardown needs it: a surviving carrier would later wake on a
// condvar whose memory the JIT has recycled. No-op if nothing was started.
void __cajeta_task_shutdown(void) {
    // The kernel pool tears down FIRST and unconditionally: it starts on the
    // first fanned-out @Kernel launch, whether or not a task carrier ever did.
    __cajeta_xpu_kpool_shutdown();
    pthread_mutex_lock(&__cajeta_task_mutex);
    if (!__cajeta_task_workers_started) {
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return;
    }
    __cajeta_task_shutdown_requested = 1;
    pthread_cond_broadcast(&__cajeta_task_queue_cond);
    int n = __cajeta_carrier_count;
    // Timer thread, if lazy-started: same recycled-condvar hazard as carriers.
    int timer_was_started = __cajeta_timer_started;
    if (timer_was_started) {
        __cajeta_timer_shutdown_requested = 1;
        pthread_cond_signal(&__cajeta_timer_cond);
    }
    // The reactor observes the flag within its 1s epoll timeout; closing the
    // epfd from outside would race its epoll_wait return.
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
    pthread_mutex_lock(&__cajeta_task_mutex);
    // Free fibers still parked: their stacks would leak, and a survivor would be
    // re-readied into a recycled or unmapped JIT context on the next run.
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

    // Tear down the net-reactor lifecycle OUTSIDE __cajeta_task_mutex — it takes
    // its own lifecycle mutex, and the two lock domains stay disjoint.
    __cajeta_net_reactor_shutdown();
}

// Enqueues a trampoline/arg pair as a fresh fiber; stack + ucontext init waits
// for the carrier's first dispatch. `fiber_slot` is the Task's fiber field.
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
    f->shadow.top = 0;
    // Inherit-on-spawn: a DEEP copy of the spawner's FiberLocal chain, so the
    // child's lifetime does not depend on the spawner's pop order.
    f->fl_top = __cajeta_fiber_local_snapshot_current();
    f->arena = (cajeta_arena) { NULL, 0, 0, 0, 0 };  // lazily mapped on first use
    f->home_carrier = -1;   // assigned on first dispatch (see carrier_loop)
    if (fiber_slot) *fiber_slot = f;

    pthread_mutex_lock(&__cajeta_task_mutex);
    // Assign a stable debug id and add to the live-fiber registry. register()
    // locks the registry mutex INSIDE the task mutex; nothing locks the reverse.
    f->dbg_id = (int) ++__cajeta_dbg_fiber_id_counter;
    __cajeta_dbg_fiber_register(f);
    if (!__cajeta_task_workers_started) {
        __cajeta_task_workers_started = 1;
        // Carrier count from $CAJETA_CARRIERS, else min(cores, cap); read once.
        int n;
        const char* env = getenv("CAJETA_CARRIERS");
        if (env && *env) {
            int parsed = atoi(env);
            n = (parsed >= 1) ? parsed : 1;
        } else {
#if defined(_WIN32)
            // sysconf/_SC_NPROCESSORS_ONLN is POSIX; ask the Win32 API instead.
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
        // Second-thread barrier, before any carrier can allocate.
        __cajeta_live_set_go_multithreaded();
        for (int i = 0; i < n; ++i) {
            pthread_create(&__cajeta_carriers[i].thread, NULL,
                           __cajeta_carrier_loop, &__cajeta_carriers[i]);
        }
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
    // publish_ready takes the target carrier's deque_mutex itself; calling it
    // outside __cajeta_task_mutex keeps the pool → deque lock order.
    __cajeta_publish_ready(f);
}

// Blocks until the task at `done_addr` flips nonzero: a fiber parks and
// re-checks (so nested await never holds the carrier hostage), the main thread
// waits on a condvar. Each wake also delivers a pending scope cancellation.
void __cajeta_task_wait(int32_t* done_addr) {
    if (!done_addr) return;
    if (__cajeta_current_fiber) {
        // Deliver a pending cancellation even when the awaited task is ALREADY
        // done: the loop below only re-checks cancel_with after a park.
        void* pending = __cajeta_current_fiber->cancel_with;
        if (pending) {
            __cajeta_current_fiber->cancel_with = NULL;
            __cajeta_throw(pending);
        }
        // Re-check *done_addr under the SAME mutex task_complete uses, then park
        // atomically — that closes the lost-wakeup window.
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

// Sets a fiber's cancel_with marker; its next task_wait resume throws it rather
// than returning. Idempotent, NULL-safe, honored at a fresh fiber's first park.
void __cajeta_fiber_cancel(struct cajeta_fiber* fiber, void* throwable) {
    if (!fiber) return;
    fiber->cancel_with = throwable;
}

// Called by the emitted trampoline once the task's value slot is written: sets
// done under the mutex, wakes awaiters, and republishes every parked fiber.
void __cajeta_task_complete(int32_t* done_addr) {
    if (!done_addr) return;
    pthread_mutex_lock(&__cajeta_task_mutex);
    // Null the Task's fiber slot BEFORE publishing done: the moment *done_addr
    // is visible the awaiter can return and the Task can be freed, so the
    // runtime must never touch Task memory after this point.
    struct cajeta_fiber* self = __cajeta_current_fiber;
    if (self && self->slot_ptr) {
        *self->slot_ptr = NULL;
        self->slot_ptr = NULL;
    }
    *done_addr = 1;
    pthread_cond_broadcast(&__cajeta_task_done_cond);
    struct cajeta_fiber* woken = __cajeta_drain_parked_locked();
    pthread_mutex_unlock(&__cajeta_task_mutex);
    // Publish woken parkers WITHOUT the pool mutex: publish_ready's deque → pool
    // order would invert against pool → deque.
    while (woken) {
        struct cajeta_fiber* next = woken->next;
        __cajeta_publish_ready(woken);
        woken = next;
    }
}

// --- R9.1 — timer wheel + cooperative timeout -----------------------------
// A deadline-sorted list under __cajeta_task_mutex walked by one timer thread.
// Entries live on the WAITING FIBER'S STACK; the fiber cancels its own.

#include <time.h>
#include <errno.h>

// The statics are declared above the carrier section (task_shutdown joins the
// thread); the struct definition follows here.
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

// Timer thread: sleeps until the next deadline or a signal, wakes expired
// fibers, sleeps again. The wrapper registers this thread's shadow stack.
static void* __cajeta_timer_loop_body(void* arg);
static void* __cajeta_timer_loop(void* arg) {
    __cajeta_prof_thread_register();
    void* r = __cajeta_timer_loop_body(arg);
    __cajeta_prof_thread_unregister();
    return r;
}
static void* __cajeta_timer_loop_body(void* arg) {
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
            // Best-effort detach: a fiber not on parked_head is already running
            // and will see the expired deadline on its next loop iteration.
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
            // A CLOCK_REALTIME timedwait against a monotonic deadline is fine
            // for an upper bound; the fiber-side check catches up either way.
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

// Blocks until *done_addr flips or the CLOCK_MONOTONIC `deadline_ns` passes;
// 1 on done, 0 on timeout. task_wait plus a per-call timer entry.
int32_t __cajeta_task_wait_timeout(int32_t* done_addr, int64_t deadline_ns) {
    if (!done_addr) return 1;
    if (!__cajeta_current_fiber) {
        // Non-fiber caller: cond_timedwait on the task-done condvar, whose clock
        // is CLOCK_REALTIME — convert the remaining monotonic nanos onto it.
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
    // Stack-local entry: the fiber's stack persists across park/resume.
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
        // Re-check both wake conditions under the mutex task_complete and the
        // timer thread use, then park atomically — the lost-wakeup bracket.
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
        // A scope's first-throw escalation may have set cancel_with while we
        // were parked; cancel our timer entry before honoring it.
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

// Fiber-aware sleep: parks on the timer wheel for `nanos` by handing
// __cajeta_task_wait_timeout a sentinel done flag that never flips.
void __cajeta_fiber_sleep_nanos(int64_t nanos) {
    if (nanos <= 0) return;
    int32_t never = 0;
    int64_t deadline = __cajeta_now_ns() + nanos;
    (void) __cajeta_task_wait_timeout(&never, deadline);
}

// --- R9.4 — I/O reactor / netpoller -----------------------------------------
// One epoll fd owned by a reactor thread: io_wait registers (fd, events, fiber)
// and parks; the reactor wakes matching waiters. EPOLLONESHOT, Linux only.

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
    // A timed waiter lives on the WAITING FIBER'S STACK, so the reactor wake
    // path must not free it: it sets `fired` instead, which the fiber reads
    // under task_mutex. Untimed (heap) waiters keep both fields zero.
    int stack_owned;
    int fired;
};

#if defined(__linux__)

// Lost-wake instrumentation, dumped by the reactor under CAJETA_REACTOR_TRACE=1.
static int64_t __caj_rt_adds, __caj_rt_mods, __caj_rt_modfail,
               __caj_rt_events, __caj_rt_matched, __caj_rt_unmatched,
               __caj_rt_published, __caj_rt_rmfail;
static int __caj_rt_trace = -1;

static int __cajeta_io_events_to_epoll(int events) {
    int e = 0;
    if (events & CAJETA_IO_READ)  e |= EPOLLIN;
    if (events & CAJETA_IO_WRITE) e |= EPOLLOUT;
    return e | EPOLLONESHOT;
}

// Union of every LISTED waiter's interest for `fd`. A timed and a plain wait
// coexist on one fd, and arming only one direction destroys the other's
// registration — the lost wake this prevents. Caller holds task_mutex.
static int __cajeta_reactor_union_events_locked(int fd) {
    int u = 0;
    for (struct cajeta_io_waiter* w = __cajeta_reactor_waiters; w;
         w = w->next) {
        if (w->fd == fd) u |= w->events;
    }
    return u;
}

// Re-arms `fd` for the waiters that remain, or DELs when none do. EPOLLONESHOT
// leaves the registration disabled but present, so MOD is the normal path.
static void __cajeta_reactor_rearm_locked(int fd) {
    int u = __cajeta_reactor_union_events_locked(fd);
    if (u == 0) {
        epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_DEL, fd, NULL);
        return;
    }
    struct epoll_event ep;
    ep.events = __cajeta_io_events_to_epoll(u);
    ep.data.fd = fd;
    if (epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_MOD, fd, &ep) < 0
            && errno == ENOENT) {
        epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_ADD, fd, &ep);
    }
}

// Reactor thread: epoll_wait with a 1s timeout so shutdown is seen with no I/O
// in flight; each event detaches matched fibers and publishes them.
static void* __cajeta_reactor_loop_body(void* arg);
static void* __cajeta_reactor_loop(void* arg) {
    __cajeta_prof_thread_register();
    void* r = __cajeta_reactor_loop_body(arg);
    __cajeta_prof_thread_unregister();
    return r;
}
static void* __cajeta_reactor_loop_body(void* arg) {
    (void) arg;
    for (;;) {
        if (__cajeta_reactor_shutdown_requested) return NULL;
        struct epoll_event ep[64];
        int n = epoll_wait(__cajeta_reactor_epfd, ep, 64, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        if (__caj_rt_trace < 0) {
            const char* t = getenv("CAJETA_REACTOR_TRACE");
            __caj_rt_trace = (t && t[0] == '1') ? 1 : 0;
        }
        if (__caj_rt_trace && n > 0) {
            fprintf(stderr, "[rt] batch n=%d ev=%lld match=%lld unmatch=%lld "
                    "pub=%lld rmfail=%lld add=%lld mod=%lld modfail=%lld\n",
                    n,
                    (long long) __caj_rt_events, (long long) __caj_rt_matched,
                    (long long) __caj_rt_unmatched, (long long) __caj_rt_published,
                    (long long) __caj_rt_rmfail, (long long) __caj_rt_adds,
                    (long long) __caj_rt_mods, (long long) __caj_rt_modfail);
        }
        if (n == 0) continue;
        struct cajeta_fiber* to_publish = NULL;
        pthread_mutex_lock(&__cajeta_task_mutex);
        for (int i = 0; i < n; ++i) {
            int fd = ep[i].data.fd;
            int __matched_this = 0;
            __caj_rt_events++;
            if (__caj_rt_trace == 1) {
                fprintf(stderr, "[ev] fd=%d bits=0x%x\n", fd,
                        (unsigned) ep[i].events);
            }
            // Which DIRECTIONS fired. ERR/HUP wake every waiter on the
            // fd — both directions' syscalls will surface the error.
            int fired_mask = 0;
            if (ep[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP))
                fired_mask |= CAJETA_IO_READ;
            if (ep[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
                fired_mask |= CAJETA_IO_WRITE;
            struct cajeta_io_waiter** p = &__cajeta_reactor_waiters;
            while (*p) {
                if ((*p)->fd == fd && ((*p)->events & fired_mask)) {
                    __matched_this = 1;
                    __caj_rt_matched++;
                    struct cajeta_io_waiter* w = *p;
                    *p = w->next;
                    // EPOLLONESHOT disarmed the registration; the re-arm below
                    // restores it for whatever waiters remain.
                    w->fired = 1;
                    if (__cajeta_parked_remove_locked(w->fiber)) {
                        w->fiber->next = to_publish;
                        to_publish = w->fiber;
                        __caj_rt_published++;
                    } else {
                        __caj_rt_rmfail++;
                    }
                    if (!w->stack_owned) free(w);
                } else {
                    p = &(*p)->next;
                }
            }
            if (!__matched_this) __caj_rt_unmatched++;
            // ONESHOT disabled the fd; waiters that did not match must not be
            // left on a dead arm.
            __cajeta_reactor_rearm_locked(fd);
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
    // A same-fd peer may still be armed, so re-arm for the remaining union: an
    // unconditional DEL destroyed its registration and parked it forever.
    __cajeta_reactor_rearm_locked(w->fd);
}

// Parks the calling fiber until a requested event fires on `fd`; 1, or -1 on
// setup failure. A non-fiber caller does a direct blocking epoll_wait instead.
int32_t __cajeta_io_wait(int32_t fd, int32_t events) {
    if (!__cajeta_current_fiber) {
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
    if (__caj_rt_trace == 1) {
        fprintf(stderr, "[iw] enter fd=%d ev=%d fib=%p\n", fd, events,
                (void*) __cajeta_current_fiber);
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
    // Arm the UNION of every listed waiter's interest for this fd: a MOD with
    // only the newcomer's direction silently disarmed a same-fd peer.
    ep.events = __cajeta_io_events_to_epoll(
        __cajeta_reactor_union_events_locked(fd));
    ep.data.fd = fd;
    int rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_ADD, fd, &ep);
    if (rc >= 0) __caj_rt_adds++;
    if (rc < 0 && errno == EEXIST) {
        rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_MOD, fd, &ep);
        if (rc >= 0) __caj_rt_mods++; else __caj_rt_modfail++;
    }
    if (rc < 0) {
        __cajeta_reactor_cancel_locked(w);
        free(w);
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return -1;
    }
    // Park while STILL holding task_mutex (park_locked releases it): the waiter
    // was registered under this same lock, so the reactor cannot fire and free
    // it before we reach parked_head — a missed one-shot wake hangs the fiber.
    __cajeta_fiber_park_locked();
    if (__caj_rt_trace == 1) {
        fprintf(stderr, "[iw] woke fd=%d fib=%p\n", fd,
                (void*) __cajeta_current_fiber);
    }
    return 1;
}

// Deadline-bounded fiber I/O wait: 1 ready, 0 deadline elapsed first, -1 setup
// error. Arms a reactor waiter and a timer entry — both on THIS FIBER'S STACK,
// both under one task_mutex — so reactor, timer and cancel each fire once.
int32_t __cajeta_io_wait_timed(int32_t fd, int32_t events, int32_t timeout_ms) {
    if (timeout_ms < 0) {
        // Unbounded: the plain park path already has the right semantics.
        return __cajeta_io_wait(fd, events);
    }
    if (!__cajeta_current_fiber) {
        // Non-fiber caller: a throwaway epoll with the deadline.
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
    ep.events = __cajeta_io_events_to_epoll(
        __cajeta_reactor_union_events_locked(fd));
    ep.data.fd = fd;
    int rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_ADD, fd, &ep);
    if (rc >= 0) __caj_rt_adds++;
    if (rc < 0 && errno == EEXIST) {
        rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_MOD, fd, &ep);
        if (rc >= 0) __caj_rt_mods++; else __caj_rt_modfail++;
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
        // Re-check both wake conditions under the mutex the reactor and timer
        // threads use, then park atomically.
        pthread_mutex_lock(&__cajeta_task_mutex);
        if (w.fired) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_timer_cancel(&entry);
            return 1;
        }
        if (__cajeta_now_ns() >= deadline_ns) {
            // Deadline first: withdraw the stack-owned waiter so the reactor can
            // never touch this frame after we return, and clear the arm.
            __cajeta_reactor_cancel_locked(&w);
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_timer_cancel(&entry);
            return 0;
        }
        __cajeta_fiber_park_locked();  // releases the mutex, then swaps
        // Honor a cancellation delivered while parked — withdraw BOTH entries
        // first, since they are stack memory about to unwind with the throw.
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

// Linux eventfd surface, for test bring-up and cross-fiber signalling: a
// counter the kernel makes edge-sensitive on write (the one-shot ready pattern).
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

// Wakes every fiber parked on `fd`, then closes it. Must be used in place of a
// bare close(2): closing removes the fd from the epoll interest list SILENTLY
// (epoll(7) Q6), so a parked fiber would never be published again.
int32_t __cajeta_io_close_fd(int32_t fd) {
    if (fd < 0) return 0;
    struct cajeta_fiber* to_publish = NULL;
    pthread_mutex_lock(&__cajeta_task_mutex);
    if (__cajeta_reactor_started) {
        // Drop the registration while the descriptor is still valid.
        epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_DEL, fd, NULL);
    }
    struct cajeta_io_waiter** p = &__cajeta_reactor_waiters;
    while (*p) {
        struct cajeta_io_waiter* w = *p;
        if (w->fd == fd) {
            *p = w->next;
            // Same protocol as the reactor's ready path: mark fired so a
            // stack-owned waiter's fiber can tell, and free only heap waiters.
            w->fired = 1;
            if (__cajeta_parked_remove_locked(w->fiber)) {
                w->fiber->next = to_publish;
                to_publish = w->fiber;
            }
            if (!w->stack_owned) free(w);
        } else {
            p = &w->next;
        }
    }
    // The close happens UNDER task_mutex, and that is the point: io_wait arms
    // its waiter and parks under this same mutex, so a waiter armed between the
    // walk and the close can no longer be orphaned on a dead descriptor.
    int r = close(fd);
    pthread_mutex_unlock(&__cajeta_task_mutex);
    while (to_publish) {
        struct cajeta_fiber* next = to_publish->next;
        __cajeta_publish_ready(to_publish);
        to_publish = next;
    }
    return (int32_t) r;
}

int32_t __cajeta_fd_close(int32_t fd) {
    return __cajeta_io_close_fd(fd);
}

#else /* !__linux__ */

int32_t __cajeta_io_close_fd(int32_t fd) { (void) fd; return -1; }


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
