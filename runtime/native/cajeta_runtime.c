// Cajeta language runtime — compiled to LLVM bitcode at compiler build time,
// embedded into the compiler binary, and linker-merged into every user module.
//
// Keep these helpers small and pointer-only at their ABI boundary; the optimizer
// inlines and specializes them across user code.

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <execinfo.h>

typedef void (*cajeta_ctor_fn)(void* self);

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
    return hdr;
}

void __cajeta_free_array(void* ptr) {
    free(ptr);
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
    return p;
}

// Mirror of __cajeta_free_array for non-array heap blocks. Kept as a
// separate symbol so the drop-fn function-pointer types match what the
// emitted IR uses for arrays (both are `void(*)(void*)`).
void __cajeta_free(void* ptr) {
    free(ptr);
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

#include <ucontext.h>
#include <string.h>

#define CAJETA_FIBER_STACK_SIZE (64 * 1024)

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
    // R5-C: cancellation marker. When non-NULL, the fiber's next
    // __cajeta_task_wait resume will throw this Throwable* instead of
    // returning normally. Set by __cajeta_fiber_cancel from scope's
    // first-throw escalation; cleared by the await re-raise path so
    // the same cancel doesn't fire twice on a fiber that survives.
    void* cancel_with;
};

static pthread_mutex_t __cajeta_task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __cajeta_task_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  __cajeta_task_done_cond  = PTHREAD_COND_INITIALIZER;
// Ready queue: fibers whose next scheduler tick will resume execution.
static struct cajeta_fiber* __cajeta_ready_head = NULL;
static struct cajeta_fiber* __cajeta_ready_tail = NULL;
// Parked list: fibers blocked inside __cajeta_task_wait waiting for some
// task's done flag to flip. Wake-all-on-any-complete is the v1 strategy.
static struct cajeta_fiber* __cajeta_parked_head = NULL;
static int __cajeta_task_workers_started = 0;
static pthread_t __cajeta_task_worker;

// Per-OS-thread state. Only the carrier thread will ever have a non-null
// __cajeta_current_fiber — the main thread sees it null and falls through
// to the cond_wait path in __cajeta_task_wait.
static __thread struct cajeta_fiber* __cajeta_current_fiber = NULL;
static __thread ucontext_t __cajeta_carrier_ctx;

// Scope chain for code running outside a fiber (the main entry point, or
// any pthread that isn't a carrier-hosted fiber). Fibers carry their own
// scope chain in `cajeta_fiber::scope_top`; the helper below picks the
// right one based on current context.
static __thread struct cajeta_scope_frame* __cajeta_main_scope_top = NULL;

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

// Fiber entry trampoline — invoked by makecontext on first resume. Runs
// the user trampoline (which calls the async fn + signals done) and then
// falls through to uc_link, which returns to the carrier.
static void __cajeta_fiber_entry(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    f->trampoline(f->trampoline_arg);
    f->state = CAJETA_FIBER_DONE;
    // Falling off the end here triggers uc_link -> back to carrier.
}

// Move every parked fiber to the ready queue. Caller holds the mutex.
// Called from __cajeta_task_complete — the cheapest correct policy when
// we don't track who-awaits-whom yet. Each woken parker will spin-check
// its own done flag and (likely) re-park if it wasn't the target.
static void __cajeta_wake_all_parked_locked(void) {
    while (__cajeta_parked_head) {
        struct cajeta_fiber* f = __cajeta_parked_head;
        __cajeta_parked_head = f->next;
        f->state = CAJETA_FIBER_READY;
        f->next = NULL;
        if (__cajeta_ready_tail) {
            __cajeta_ready_tail->next = f;
            __cajeta_ready_tail = f;
        } else {
            __cajeta_ready_head = f;
            __cajeta_ready_tail = f;
        }
    }
    pthread_cond_broadcast(&__cajeta_task_queue_cond);
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
    swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// Carrier loop: pop a ready fiber, swap into it, observe its post-yield
// state, free if done. Single carrier suffices for v1 because all fibers
// are cooperative — they yield to each other inside this thread.
static void* __cajeta_carrier_loop(void* arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&__cajeta_task_mutex);
        while (!__cajeta_ready_head) {
            pthread_cond_wait(&__cajeta_task_queue_cond, &__cajeta_task_mutex);
        }
        struct cajeta_fiber* f = __cajeta_ready_head;
        __cajeta_ready_head = f->next;
        if (!__cajeta_ready_head) __cajeta_ready_tail = NULL;
        f->next = NULL;
        pthread_mutex_unlock(&__cajeta_task_mutex);

        __cajeta_current_fiber = f;
        f->state = CAJETA_FIBER_RUNNING;
        if (!f->stack) {
            // First-resume init: allocate the stack and prime the context
            // to dispatch to __cajeta_fiber_entry. uc_link points back at
            // the carrier so a normal fall-off-the-end returns here.
            f->stack = malloc(CAJETA_FIBER_STACK_SIZE);
            if (!f->stack) {
                fprintf(stderr, "cajeta: fiber stack malloc failed\n");
                abort();
            }
            getcontext(&f->ctx);
            f->ctx.uc_stack.ss_sp = f->stack;
            f->ctx.uc_stack.ss_size = CAJETA_FIBER_STACK_SIZE;
            f->ctx.uc_link = &__cajeta_carrier_ctx;
            makecontext(&f->ctx, __cajeta_fiber_entry, 0);
        }
        swapcontext(&__cajeta_carrier_ctx, &f->ctx);
        __cajeta_current_fiber = NULL;
        if (f->state == CAJETA_FIBER_DONE) {
            free(f->stack);
            free(f);
        }
        // Parked fibers stay on __cajeta_parked_head awaiting a wake.
    }
    return NULL;
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
    if (fiber_slot) *fiber_slot = f;

    pthread_mutex_lock(&__cajeta_task_mutex);
    if (!__cajeta_task_workers_started) {
        __cajeta_task_workers_started = 1;
        pthread_create(&__cajeta_task_worker, NULL,
                       __cajeta_carrier_loop, NULL);
    }
    if (__cajeta_ready_tail) {
        __cajeta_ready_tail->next = f;
        __cajeta_ready_tail = f;
    } else {
        __cajeta_ready_head = f;
        __cajeta_ready_tail = f;
    }
    pthread_cond_signal(&__cajeta_task_queue_cond);
    pthread_mutex_unlock(&__cajeta_task_mutex);
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
        while (!*done_addr) {
            __cajeta_fiber_park();
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
    pthread_mutex_lock(&__cajeta_task_mutex);
    *done_addr = 1;
    pthread_cond_broadcast(&__cajeta_task_done_cond);
    __cajeta_wake_all_parked_locked();
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// --- Threading: scope frames (R5-A) ---------------------------------------
//
// `scope { ... }` blocks own their child tasks: codegen emits a
// __cajeta_scope_enter at the opening brace, every spawn site inside
// calls __cajeta_scope_register with its task's done-addr, and the
// closing brace runs __cajeta_scope_exit which waits for every
// registered task's done flag before letting control past `}`.
//
// The scope chain is per-fiber (fibers running on the same carrier OS
// thread must not see each other's scopes); the main thread has its
// own TLS chain. The __cajeta_scope_top_ptr helper picks the right
// slot transparently.

void __cajeta_scope_enter(void) {
    struct cajeta_scope_frame* f =
        (struct cajeta_scope_frame*) malloc(sizeof(*f));
    if (!f) {
        fprintf(stderr, "cajeta: __cajeta_scope_enter malloc failed\n");
        abort();
    }
    f->entries = NULL;
    f->count = 0;
    f->cap = 0;
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    f->prev = *top;
    *top = f;
}

// Append a task's (done, exception) pair to the current scope frame.
// spawn sites call this just before __cajeta_task_run; the corresponding
// scope_exit waits on done then inspects exception. Doc behavior: spawn
// outside any scope is a compile error — but today's MVP doesn't enforce
// that, so register-without-scope is a no-op rather than an abort
// (preserving the existing tests' top-level-spawn pattern).
void __cajeta_scope_register(int32_t* done_addr, void** exception_addr,
                             void** fiber_slot) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    struct cajeta_scope_frame* f = *top;
    if (!f) return;
    if (f->count == f->cap) {
        int newcap = f->cap ? f->cap * 2 : 4;
        struct cajeta_scope_entry* grown =
            (struct cajeta_scope_entry*) realloc(f->entries,
                newcap * sizeof(struct cajeta_scope_entry));
        if (!grown) {
            fprintf(stderr, "cajeta: __cajeta_scope_register realloc failed\n");
            abort();
        }
        f->entries = grown;
        f->cap = newcap;
    }
    f->entries[f->count].done_addr = done_addr;
    f->entries[f->count].exception_addr = exception_addr;
    f->entries[f->count].fiber_slot = fiber_slot;
    f->count++;
}

// Wait for every registered task's done flag, then walk again checking
// exception slots. If any task threw, re-raise the first one found —
// the doc's "first-throw wins" semantics for scope joins.
//
// R5-D-lite: no sibling cancellation yet. Pre-cancel R5-C lands, this
// just waits for every child to complete naturally, then escalates.
// When R5-C lands, the loop becomes: on first non-null exception,
// cancel the rest, wait for them, then re-raise.
static void __cajeta_scope_release(struct cajeta_scope_frame* f) {
    free(f->entries);
    free(f);
}

void __cajeta_scope_exit(void) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    struct cajeta_scope_frame* f = *top;
    if (!f) return;
    // First-throw-wins escalation with R5-C cancellation: wait on each
    // child in turn; the moment one's exception slot is non-null, mark
    // that as the trigger and cancel every remaining (unawaited) child
    // so their next await aborts. Then keep waiting on the rest so the
    // scope still joins everything before unwinding upward.
    void* trigger = NULL;
    for (int i = 0; i < f->count; i++) {
        __cajeta_task_wait(f->entries[i].done_addr);
        if (!trigger && f->entries[i].exception_addr
                && *f->entries[i].exception_addr) {
            trigger = *f->entries[i].exception_addr;
            for (int j = i + 1; j < f->count; j++) {
                if (f->entries[j].fiber_slot && *f->entries[j].fiber_slot) {
                    __cajeta_fiber_cancel(
                        (struct cajeta_fiber*) *f->entries[j].fiber_slot,
                        trigger);
                }
            }
        }
    }
    *top = f->prev;
    __cajeta_scope_release(f);
    if (trigger) {
        __cajeta_throw(trigger);
    }
}

// Watermark API for the R5-A' implicit function-body scope. Codegen
// captures the scope_top observed at function entry, then calls
// __cajeta_scope_exit_to with that watermark on every return path.
// Pops every frame above the watermark in LIFO order — handles the
// case where `return` exits from inside one or more explicit
// `scope { }` blocks nested under the function body's implicit scope.
void* __cajeta_scope_save_top(void) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    return (void*) *top;
}

void __cajeta_scope_exit_to(void* watermark) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    // Collect the first trigger encountered while popping frames so we
    // can re-raise it at the end. Walking innermost-out matches "child
    // exceptions surface up to the enclosing scope"; the topmost frame's
    // throws win over outer frames because we process them first. R5-C:
    // cancel remaining siblings within each frame as soon as we see a
    // trigger from it, same as scope_exit.
    void* trigger = NULL;
    while (*top != (struct cajeta_scope_frame*) watermark) {
        struct cajeta_scope_frame* f = *top;
        if (!f) break;
        void* frame_trigger = NULL;
        for (int i = 0; i < f->count; i++) {
            __cajeta_task_wait(f->entries[i].done_addr);
            if (!frame_trigger && f->entries[i].exception_addr
                    && *f->entries[i].exception_addr) {
                frame_trigger = *f->entries[i].exception_addr;
                for (int j = i + 1; j < f->count; j++) {
                    if (f->entries[j].fiber_slot && *f->entries[j].fiber_slot) {
                        __cajeta_fiber_cancel(
                            (struct cajeta_fiber*) *f->entries[j].fiber_slot,
                            frame_trigger);
                    }
                }
            }
        }
        if (!trigger && frame_trigger) trigger = frame_trigger;
        *top = f->prev;
        __cajeta_scope_release(f);
    }
    if (trigger) {
        __cajeta_throw(trigger);
    }
}

// --- Threading sync primitives: Lock (async-aware, R4) -------------------
//
// Sits on top of the fiber executor. A Cajeta Lock is a pthread_mutex_t
// for state protection plus a per-lock wait queue of parked fibers and a
// condvar for any main-thread waiter. Acquire from a fiber that hits a
// held lock parks the fiber on the lock's queue and yields — the carrier
// keeps running other fibers. Acquire from the main thread cond_waits
// like a regular pthread (main is outside the fiber executor and can OS-
// block without harming concurrency).
//
// `held` is the canonical "is anyone holding this lock" flag, distinct
// from the pthread_mutex_t state (which protects the lock's own metadata
// — held + wait queue — not user mutual exclusion). Splitting them lets
// the fiber path park on the wait queue without leaving the user-mutex
// held.

struct cajeta_async_lock {
    pthread_mutex_t mutex;
    pthread_cond_t released_cond;
    int held;
    struct cajeta_fiber* wait_head;
    struct cajeta_fiber* wait_tail;
};

void* __cajeta_lock_new(void) {
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) malloc(sizeof(*l));
    if (!l) {
        fprintf(stderr, "cajeta: __cajeta_lock_new failed\n");
        abort();
    }
    if (pthread_mutex_init(&l->mutex, NULL) != 0) {
        fprintf(stderr, "cajeta: pthread_mutex_init failed\n");
        free(l);
        abort();
    }
    if (pthread_cond_init(&l->released_cond, NULL) != 0) {
        fprintf(stderr, "cajeta: pthread_cond_init failed\n");
        pthread_mutex_destroy(&l->mutex);
        free(l);
        abort();
    }
    l->held = 0;
    l->wait_head = NULL;
    l->wait_tail = NULL;
    return l;
}

void __cajeta_lock_acquire(void* p) {
    if (!p) return;
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) p;
    if (!__cajeta_current_fiber) {
        // Main thread (or any non-fiber pthread): cond_wait until the
        // holder releases. Matches the doc's intent that the program's
        // entry point can sit on a top-level await without burning CPU.
        pthread_mutex_lock(&l->mutex);
        while (l->held) {
            pthread_cond_wait(&l->released_cond, &l->mutex);
        }
        l->held = 1;
        pthread_mutex_unlock(&l->mutex);
        return;
    }
    // Fiber path: park on the lock's wait queue if held. On wake the
    // dequeued-by-release fiber is back on the ready queue; when the
    // carrier dispatches it, swapcontext returns INTO this loop and
    // re-checks held (another acquirer could have raced in, so re-park
    // rather than assume we have the lock).
    for (;;) {
        pthread_mutex_lock(&l->mutex);
        if (!l->held) {
            l->held = 1;
            pthread_mutex_unlock(&l->mutex);
            return;
        }
        struct cajeta_fiber* self = __cajeta_current_fiber;
        self->next = NULL;
        if (l->wait_tail) {
            l->wait_tail->next = self;
            l->wait_tail = self;
        } else {
            l->wait_head = self;
            l->wait_tail = self;
        }
        self->state = CAJETA_FIBER_PARKED;
        pthread_mutex_unlock(&l->mutex);
        swapcontext(&self->ctx, &__cajeta_carrier_ctx);
    }
}

void __cajeta_lock_release(void* p) {
    if (!p) return;
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) p;
    pthread_mutex_lock(&l->mutex);
    l->held = 0;
    struct cajeta_fiber* next = l->wait_head;
    if (next) {
        l->wait_head = next->next;
        if (!l->wait_head) l->wait_tail = NULL;
        next->next = NULL;
    }
    // Signal one main-thread waiter even when a fiber is being handed
    // the lock — costs almost nothing and avoids missed-wake races with
    // a pthread that happens to be cond_waiting concurrently.
    pthread_cond_signal(&l->released_cond);
    pthread_mutex_unlock(&l->mutex);
    if (next) {
        // Move the woken fiber onto the carrier's ready queue. Done
        // under the task mutex so it pairs correctly with the carrier's
        // dequeue.
        pthread_mutex_lock(&__cajeta_task_mutex);
        next->state = CAJETA_FIBER_READY;
        next->next = NULL;
        if (__cajeta_ready_tail) {
            __cajeta_ready_tail->next = next;
            __cajeta_ready_tail = next;
        } else {
            __cajeta_ready_head = next;
            __cajeta_ready_tail = next;
        }
        pthread_cond_signal(&__cajeta_task_queue_cond);
        pthread_mutex_unlock(&__cajeta_task_mutex);
    }
}

// Returns 1 if acquired, 0 if already held. Non-blocking even on a
// fiber — `tryAcquire` semantics are "give me the lock right now or
// tell me you couldn't", never "park me".
int32_t __cajeta_lock_try_acquire(void* p) {
    if (!p) return 0;
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) p;
    int got = 0;
    pthread_mutex_lock(&l->mutex);
    if (!l->held) {
        l->held = 1;
        got = 1;
    }
    pthread_mutex_unlock(&l->mutex);
    return got;
}

void __cajeta_lock_destroy(void* p) {
    if (!p) return;
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) p;
    pthread_cond_destroy(&l->released_cond);
    pthread_mutex_destroy(&l->mutex);
    free(l);
}

// Abort with a diagnostic when an array index is out of bounds. Compiler emits a
// conditional branch to this from ArrayIndexExpression when bounds checking is on.
void __cajeta_array_bounds_fail(int64_t index, int64_t dim) {
    fprintf(stderr, "cajeta: array index %lld out of bounds for dimension size %lld\n",
            (long long) index, (long long) dim);
    abort();
}

// --- exception handling (setjmp/longjmp-based) -------------------------------
//
// Each try-block allocates a `cajeta_exception_frame` on the stack and registers
// it with __cajeta_exc_push. `throw` writes the value into the topmost frame and
// longjmps back to its setjmp point. The compiler emits the setjmp call inline
// (it must run in the caller's frame), so the runtime never sees it directly.
//
// Per-thread: each OS thread has its own `__cajeta_main_exc_top` __thread
// slot; each carrier-hosted fiber owns its own slot inside `cajeta_fiber`.
// `__cajeta_exc_top_ptr` picks the right one based on whether the call
// site is running inside a fiber. The drop-chain head uses the same model.

// --- drop chain (Session 3 of the memory-model rollout) ---------------------
//
// Owners declared in a function push a `cajeta_drop_entry` onto a per-thread
// linked list at declaration time, and pop+drop at scope exit. The exception
// throw path walks this chain down to the catching try-frame's watermark so
// stack unwinding fires drops along the way. See MemoryModel.md § Runtime:
// drop chain with watermark.

struct cajeta_drop_entry {
    void* obj;
    void (*drop_fn)(void*);
    struct cajeta_drop_entry* prev;
    int8_t active;  // i8 instead of bool — fixed ABI for the IR side
};

size_t __cajeta_drop_entry_size(void) {
    return sizeof(struct cajeta_drop_entry);
}

// Drop chain head — per-thread (main has its own __thread slot; carrier-
// hosted fibers each own a slot inside their cajeta_fiber struct). Before
// this rollout this was a single global static. With the carrier thread
// running concurrently with main on shared globals, even a "single-
// carrier" model races on the head pointer. Promoting to per-thread/
// per-fiber is the doc's planned shape (AsyncStatus.md § TLS-promote
// the exception chain + drop-chain head).
static __thread struct cajeta_drop_entry* __cajeta_main_drop_top = NULL;

// Returns a pointer to the current drop_top slot — either the running
// fiber's slot or the main thread's TLS slot. Callers read or write
// through this pointer, so push/pop works uniformly regardless of
// fiber vs main context. Mirrors __cajeta_scope_top_ptr.
static struct cajeta_drop_entry** __cajeta_drop_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->drop_top;
    }
    return &__cajeta_main_drop_top;
}

// Observability for tests: bumped every time a drop function actually fires
// (i.e. an active entry pops or unwinds). Tests can read it to assert that
// owned resources are freed at expected program points. Atomic so drops
// fired on the carrier thread are visible to tests reading from main.
static int64_t __cajeta_drop_count = 0;

int64_t __cajeta_drop_count_get(void) {
    return __atomic_load_n(&__cajeta_drop_count, __ATOMIC_SEQ_CST);
}
void __cajeta_drop_count_reset(void) {
    __atomic_store_n(&__cajeta_drop_count, 0, __ATOMIC_SEQ_CST);
}

// Push an owner onto the drop chain. The entry storage is stack-allocated in
// the caller's frame; we never own the memory, only chain pointers through it.
void __cajeta_drop_push(struct cajeta_drop_entry* e, void* obj, void (*drop_fn)(void*)) {
    struct cajeta_drop_entry** top = __cajeta_drop_top_ptr();
    e->obj = obj;
    e->drop_fn = drop_fn;
    e->prev = *top;
    e->active = 1;
    *top = e;
}

// Pop the topmost entry and run its drop function if still active. Caller
// passes the entry pointer so popping can verify shape (in debug builds; v1
// trusts the caller).
void __cajeta_drop_pop_run(struct cajeta_drop_entry* e) {
    struct cajeta_drop_entry** top = __cajeta_drop_top_ptr();
    if (e->active && e->drop_fn) {
        __atomic_fetch_add(&__cajeta_drop_count, 1, __ATOMIC_SEQ_CST);
        e->drop_fn(e->obj);
    }
    *top = e->prev;
}

// Mark an entry inactive (the owner has been moved out via `#`). The entry
// remains on the chain so scope-exit pop logic still finds it, but the drop
// function won't run.
void __cajeta_drop_mark_inactive(struct cajeta_drop_entry* e) {
    e->active = 0;
}

// --- VTable: hash-based dispatch ---------------------------------------------
//
// Each class's vtable is a sorted array of (signature-hash, function-pointer)
// entries. Dispatch hashes the call-site's method canonical signature, binary-
// searches the receiver's vtable, and indirect-calls the matching function.
// This sidesteps the slot-index collision problem that single-vtable layouts
// run into for multiple inheritance — methods are addressed by stable hash,
// not by position.
//
// Layout (LLVM struct):
//   { i16 version, i16 count, [count x { i64 hash, ptr fn }] entries }
//
// The header is 4 bytes (`version` + `count`), but the entries array is
// 8-byte aligned per LLVM's default rules — so there are 4 bytes of padding
// before `entries`, and the entries themselves start at byte offset 8.

struct cajeta_vtable_entry {
    int64_t hash;
    void* fn;
};

// FNV-1a 64-bit hash. Stable across runs and platforms — both the compiler
// (at vtable build time) and the runtime (at dispatch time) compute the
// same hash for the same canonical signature.
int64_t __cajeta_signature_hash(const char* s) {
    if (!s) return 0;
    uint64_t h = 0xcbf29ce484222325ULL;     // FNV offset basis
    while (*s) {
        h ^= (uint8_t) *s++;
        h *= 0x100000001b3ULL;              // FNV prime
    }
    return (int64_t) h;
}

// Binary-search the vtable for `hash`; return the matching function pointer
// or NULL if not found. The "not found" case shouldn't happen for well-typed
// dispatch — the static type guarantees the method exists on the receiver —
// but is treated as a soft miss so a misuse aborts at the call site (NULL
// fn-pointer call) rather than corrupting memory.
// VTable byte layout (kept in sync with StructureMetadata::createVirtualTableType):
//   [0..1]   i16 version
//   [2..3]   i16 count
//   [4..7]   pad (LLVM auto-inserts to align ptr to 8 bytes)
//   [8..15]  ptr parent_vtable        (NULL at root)
//   [16..]   [count x { i64 hash, ptr fn }] entries
#define CAJETA_VTABLE_PARENT_OFFSET 8
#define CAJETA_VTABLE_ENTRIES_OFFSET 16

void* __cajeta_vtable_lookup(void* vptr, int64_t hash) {
    if (!vptr) return NULL;
    const int16_t* hdr = (const int16_t*) vptr;
    int32_t count = hdr[1];                 // slot 1 = count
    if (count <= 0) return NULL;
    const struct cajeta_vtable_entry* entries =
        (const struct cajeta_vtable_entry*)
            ((const char*) vptr + CAJETA_VTABLE_ENTRIES_OFFSET);
    int lo = 0, hi = count;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        int64_t h = entries[mid].hash;
        if (h < hash) lo = mid + 1;
        else if (h > hash) hi = mid;
        else return entries[mid].fn;
    }
    return NULL;
}

// Marker global set by codegen to UnrecoverableException's vtable address.
// __cajeta_is_unrecoverable compares each ancestor vtable against this.
// `extern weak` here; the user module's compilation emits the strong
// definition with its initializer pointing at
// cajeta.lang.UnrecoverableException#VTable. The weak attribute lets the
// native test-binary link (which doesn't go through emitUnrecoverableMarker)
// resolve the symbol to NULL — the runtime null-checks it below.
extern void* __cajeta_unrecoverable_vtable_marker __attribute__((weak));

// Walk a Throwable's vtable chain to determine whether it's an
// UnrecoverableException (or any descendant thereof). Returns 1 if so,
// 0 otherwise. Driven by the parent_vtable pointer at offset 8 of each
// vtable global; walks until either a match is found or the chain hits
// NULL (root).
int32_t __cajeta_is_unrecoverable(void* throwable) {
    if (!throwable) return 0;
    // Defensive: the legacy `throw 42` idiom IntToPtrs an integer into
    // the runtime; the resulting "pointer" is the integer's bit pattern
    // and is virtually never a real heap address. Dereferencing it for
    // the vtable read would SIGSEGV. Filter anything below the
    // typical zero-page boundary so we treat legacy int throws as
    // (definitely-not-)Unrecoverable. Long-term: phase out int throws
    // in favor of real Throwable instances.
    if ((uintptr_t) throwable < 4096) return 0;
    void* vtable = *(void**) throwable;   // instance slot 0 = vtable ptr
    if (!__cajeta_unrecoverable_vtable_marker) return 0;
    while (vtable) {
        if (vtable == __cajeta_unrecoverable_vtable_marker) return 1;
        vtable = *(void**) ((char*) vtable + CAJETA_VTABLE_PARENT_OFFSET);
    }
    return 0;
}

// Forward decl — defined alongside __cajeta_throw further down.
static void __cajeta_emit_uncaught(void* value, int is_unrec);

// Called by the fiber trampoline's catch block (per Error-model #205 and
// #210). If the thrown value is an Unrecoverable, print + abort the
// whole process — propagating into the Task slot would let a runtime
// invariant violation hide behind await suspension. Recoverable returns
// normally; the trampoline's caller stores it on the Task's exception
// slot for await to re-raise.
void __cajeta_fiber_handle_throw(void* thrown) {
    if (__cajeta_is_unrecoverable(thrown)) {
        __cajeta_emit_uncaught(thrown, /*is_unrec=*/1);
        abort();
    }
}

struct cajeta_exception_frame {
    jmp_buf buf;
    struct cajeta_exception_frame* prev;
    // R5/Error-model #202: the thrown value is now a void* — typed at the
    // codegen level as a Throwable*, but the runtime is type-agnostic so we
    // store it as a bare pointer. Backwards-compatible with the old int64-
    // throw idiom: ThrowStatement converts integer literals via IntToPtr,
    // TryStatement's catch binding reads back via PtrToInt when the
    // declared catch type is integer-shaped.
    void* thrown_value;
    // Drop-chain watermark snapshotted at try-entry. On throw, the runtime
    // unwinds drops between the current top and this watermark before longjmp.
    struct cajeta_drop_entry* drop_watermark;
};

// Exposed as a compile-time-known size for the IR side; the compiler allocates a
// blob of this size for each try-frame. Using a fixed 512-byte buffer in IR is
// portable enough for x86-64 and aarch64 glibc/musl, but we expose the actual
// size here so the JIT helper can sanity-check.
size_t __cajeta_exc_frame_size(void) {
    return sizeof(struct cajeta_exception_frame);
}

// Exception chain head — per-thread (main has its own __thread slot;
// carrier-hosted fibers each own a slot inside their cajeta_fiber
// struct). Same rationale as the drop chain head above. The
// drop_watermark stored in each frame snapshots the per-thread drop
// top at try-entry time, so an unwind through __cajeta_throw works
// against the same chain the user code pushed into.
static __thread struct cajeta_exception_frame* __cajeta_main_exc_top = NULL;

static struct cajeta_exception_frame** __cajeta_exc_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->exc_top;
    }
    return &__cajeta_main_exc_top;
}

void __cajeta_exc_push(struct cajeta_exception_frame* f) {
    struct cajeta_exception_frame** top = __cajeta_exc_top_ptr();
    struct cajeta_drop_entry** dropTop = __cajeta_drop_top_ptr();
    f->prev = *top;
    f->thrown_value = NULL;
    // Snapshot the current drop-chain top so a throw can unwind back to here.
    f->drop_watermark = *dropTop;
    *top = f;
}

void __cajeta_exc_pop(void) {
    struct cajeta_exception_frame** top = __cajeta_exc_top_ptr();
    if (*top) {
        *top = (*top)->prev;
    }
}

// --- R5/Error-model #203: stack-trace capture ---------------------------
//
// At every throw site, walk the native call stack via backtrace() and
// store the return-address array in a side table keyed by the throwable
// pointer. Auto-printed on uncaught throws (when the runtime aborts) and
// retrievable via __cajeta_get_trace / __cajeta_print_trace for user-
// invoked introspection. Side-table avoids the Throwable struct-layout
// problem (subclasses don't carry parent fields in their memory image
// today — see follow-up #208 — so we can't reliably stash the trace as
// a field on Throwable). When that's fixed, this can migrate to a real
// field with no API change at the Cajeta level.

struct cajeta_trace_entry {
    void* throwable;
    void** frames;
    int frame_count;
    struct cajeta_trace_entry* next;
};

static struct cajeta_trace_entry* __cajeta_trace_table = NULL;
static pthread_mutex_t __cajeta_trace_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CAJETA_TRACE_MAX_FRAMES 64

static void __cajeta_trace_record(void* throwable) {
    if (!throwable) return;
    void* buf[CAJETA_TRACE_MAX_FRAMES];
    int n = backtrace(buf, CAJETA_TRACE_MAX_FRAMES);
    if (n <= 0) return;
    void** frames = (void**) malloc((size_t) n * sizeof(void*));
    if (!frames) return;
    memcpy(frames, buf, (size_t) n * sizeof(void*));
    struct cajeta_trace_entry* e =
        (struct cajeta_trace_entry*) malloc(sizeof(*e));
    if (!e) { free(frames); return; }
    e->throwable = throwable;
    e->frames = frames;
    e->frame_count = n;
    pthread_mutex_lock(&__cajeta_trace_mutex);
    e->next = __cajeta_trace_table;
    __cajeta_trace_table = e;
    pthread_mutex_unlock(&__cajeta_trace_mutex);
}

// Print the trace for `throwable` to fd (1=stdout, 2=stderr). No-op if
// no trace was recorded for this throwable.
void __cajeta_print_trace(void* throwable, int32_t fd) {
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != throwable) e = e->next;
    if (!e) { pthread_mutex_unlock(&__cajeta_trace_mutex); return; }
    char** syms = backtrace_symbols(e->frames, e->frame_count);
    if (syms) {
        FILE* out = (fd == 1) ? stdout : stderr;
        for (int i = 0; i < e->frame_count; i++) {
            fprintf(out, "  %s\n", syms[i]);
        }
        fflush(out);
        free(syms);
    }
    pthread_mutex_unlock(&__cajeta_trace_mutex);
}

// Helper for the uncaught-throw path: print the throwable's message
// (if any) and stack trace to stderr. Mirrors Java/Python's "Exception
// in thread main: ... \n Traceback: ..." shape. Throwable layout is
// { vtable, message, ... } so the message field is at offset 8 in
// any class derived from Throwable — see ErrorModel.md § hierarchy.
// If the value isn't a Throwable instance (e.g. legacy int throw via
// IntToPtr), the message read may yield garbage; we print the raw
// pointer in that case as a hex fallback. The trace is recorded at
// throw time regardless of whether the throwable carries a message.
static void __cajeta_emit_uncaught(void* value, int is_unrec) {
    const char* kind = is_unrec ? "unrecoverable" : "uncaught";
    void* msg = NULL;
    // Same low-address guard as __cajeta_is_unrecoverable: legacy int
    // throws produce non-pointer "throwables" that we must not
    // dereference. Skip the message read in that case; print the bare
    // value as a hex fallback. Reads on legitimate Throwable instances
    // (heap-allocated, vtable + message at slots 0/1) work as expected.
    if (value && (uintptr_t) value >= 4096) {
        msg = ((void**) value)[1];
    }
    if (msg) {
        fprintf(stderr, "cajeta: %s exception: %s\n", kind, (const char*) msg);
    } else {
        fprintf(stderr, "cajeta: %s exception (value=%p)\n", kind, value);
    }
    __cajeta_print_trace(value, 2);
}

__attribute__((noreturn))
void __cajeta_throw(void* value) {
    __cajeta_trace_record(value);
    struct cajeta_exception_frame** excTop = __cajeta_exc_top_ptr();
    if (!*excTop) {
        int is_unrec = __cajeta_is_unrecoverable(value);
        __cajeta_emit_uncaught(value, is_unrec);
        if (is_unrec) {
            // Alarm semantics — abort produces a SIGABRT, dump-friendly.
            abort();
        }
        // Recoverable that escaped every handler. Exit cleanly with a
        // nonzero code. The runtime's stderr emission above is the user-
        // facing diagnostic.
        exit(1);
    }
    // Unwind drops between the current top and the catching frame's watermark.
    // Each active entry runs its drop function once; the entry itself is
    // stack-allocated in the originating frame, so we only manipulate the
    // chain pointer here.
    struct cajeta_drop_entry** dropTop = __cajeta_drop_top_ptr();
    struct cajeta_drop_entry* watermark = (*excTop)->drop_watermark;
    while (*dropTop != watermark) {
        struct cajeta_drop_entry* e = *dropTop;
        if (!e) break;  // shouldn't happen, but bail rather than loop
        if (e->active && e->drop_fn) {
            __atomic_fetch_add(&__cajeta_drop_count, 1, __ATOMIC_SEQ_CST);
            e->drop_fn(e->obj);
        }
        *dropTop = e->prev;
    }
    (*excTop)->thrown_value = value;
    longjmp((*excTop)->buf, 1);
}

void* __cajeta_get_thrown(void) {
    struct cajeta_exception_frame** top = __cajeta_exc_top_ptr();
    return *top ? (*top)->thrown_value : NULL;
}

// --- I/O helpers: print / println / log (SLF4J-style {} templating) ----------
//
// The compiler emits direct calls to these for System.{stdout,stderr,stdin}
// .{print,println,printf}(...). stream is the file descriptor: 0=stdin,
// 1=stdout, 2=stderr. Writing to stdin is unusual but supported because the
// language exposes the same surface on all three streams.

#include <unistd.h>

static void __cajeta_emit(int32_t stream, const char* s, size_t n) {
    if (!s || n == 0) return;
    // Use write() so output is unbuffered relative to the host's stdio buffers —
    // matters for tests that capture descriptor-level output.
    ssize_t r = write(stream, s, n);
    (void) r;  // best-effort; ignore short writes / EBADF
}

void __cajeta_print(int32_t stream, const char* s) {
    if (!s) return;
    __cajeta_emit(stream, s, strlen(s));
}

void __cajeta_println(int32_t stream, const char* s) {
    if (s) __cajeta_emit(stream, s, strlen(s));
    __cajeta_emit(stream, "\n", 1);
}

// Primitive overloads — the compiler picks one based on the static type of the
// argument expression. Integers are widened to i64, floats to f64. Booleans
// stringify to "true"/"false" (matching Java's PrintStream.println(boolean)).
void __cajeta_print_i64(int32_t stream, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long) v);
    if (n > 0) __cajeta_emit(stream, buf, (size_t) n);
}
void __cajeta_println_i64(int32_t stream, int64_t v) {
    __cajeta_print_i64(stream, v);
    __cajeta_emit(stream, "\n", 1);
}

void __cajeta_print_f64(int32_t stream, double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", v);
    if (n > 0) __cajeta_emit(stream, buf, (size_t) n);
}
void __cajeta_println_f64(int32_t stream, double v) {
    __cajeta_print_f64(stream, v);
    __cajeta_emit(stream, "\n", 1);
}

void __cajeta_print_bool(int32_t stream, int32_t v) {
    if (v) __cajeta_emit(stream, "true", 4);
    else   __cajeta_emit(stream, "false", 5);
}
void __cajeta_println_bool(int32_t stream, int32_t v) {
    __cajeta_print_bool(stream, v);
    __cajeta_emit(stream, "\n", 1);
}

// --- string concatenation helpers --------------------------------------------
//
// The compiler emits these when it sees `+` with at least one String operand.
// Concatenation results and stringified primitives are heap-allocated and leak
// today — Cajeta's owner/borrower memory model isn't wired through these yet.
// Acceptable for now: typical workloads concat a bounded number of times per
// log call, not in tight loops; a future pass will thread the lifetime through
// the IR.

char* __cajeta_i64_to_str(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long) v);
    if (n < 0) n = 0;
    char* out = (char*) malloc((size_t) n + 1);
    if (!out) return NULL;
    memcpy(out, buf, (size_t) n);
    out[n] = '\0';
    return out;
}

char* __cajeta_f64_to_str(double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", v);
    if (n < 0) n = 0;
    char* out = (char*) malloc((size_t) n + 1);
    if (!out) return NULL;
    memcpy(out, buf, (size_t) n);
    out[n] = '\0';
    return out;
}

// Boolean stringification returns a static literal — callers must not free it.
// All other to_str helpers return malloc'd memory; concat treats every input as
// borrowed (never frees) to keep the rule uniform.
const char* __cajeta_bool_to_str(int32_t v) {
    return v ? "true" : "false";
}

// Copy `length` bytes from `data` into a freshly malloc'd null-terminated
// string. Used by struct-view field reads on `String`-typed fields: the
// inline bytes in the buffer aren't null-terminated, so we materialize an
// owned copy that's compatible with the existing String stdlib (strlen,
// strcmp, etc.). Caller takes ownership of the result.
char* __cajeta_str_view_to_owned(const char* data, int64_t length) {
    if (length < 0) length = 0;
    char* out = (char*) malloc((size_t) length + 1);
    if (!out) return NULL;
    if (data && length > 0) memcpy(out, data, (size_t) length);
    out[length] = '\0';
    return out;
}

int64_t __cajeta_str_len(const char* s) {
    return s ? (int64_t) strlen(s) : 0;
}

// Returns 1 if both strings are non-null and byte-equal, 0 otherwise. Two nulls
// are considered NOT equal to match the principle that null is not a value —
// adjust if/when null-semantics consolidate around Java's behavior.
int32_t __cajeta_str_equals(const char* a, const char* b) {
    if (!a || !b) return 0;
    return strcmp(a, b) == 0 ? 1 : 0;
}

int32_t __cajeta_str_isEmpty(const char* s) {
    return (!s || s[0] == '\0') ? 1 : 0;
}

// charAt returns the byte at `index` as int8. Out-of-range or null returns 0
// (matches a zero-default rather than throwing — exceptions in Cajeta require a
// live try-frame, which a primitive accessor shouldn't assume).
int8_t __cajeta_str_charAt(const char* s, int64_t index) {
    if (!s || index < 0) return 0;
    size_t n = strlen(s);
    if ((size_t) index >= n) return 0;
    return (int8_t) s[index];
}

int64_t __cajeta_str_indexOf(const char* s, const char* needle) {
    if (!s || !needle) return -1;
    const char* p = strstr(s, needle);
    return p ? (int64_t) (p - s) : -1;
}

int32_t __cajeta_str_startsWith(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    size_t lp = strlen(prefix);
    return strncmp(s, prefix, lp) == 0 ? 1 : 0;
}

int32_t __cajeta_str_endsWith(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (lf > ls) return 0;
    return memcmp(s + ls - lf, suffix, lf) == 0 ? 1 : 0;
}

int32_t __cajeta_str_contains(const char* s, const char* needle) {
    if (!s || !needle) return 0;
    return strstr(s, needle) != NULL ? 1 : 0;
}

// Returns a malloc'd ASCII-uppercased copy. Non-ASCII bytes pass through
// unchanged — multibyte/UTF-8 case folding needs a real locale-aware library
// that's outside the scope of the embedded runtime today.
char* __cajeta_str_toUpperCase(const char* s) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    size_t n = strlen(s);
    char* out = (char*) malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char) (c - 32) : (char) c;
    }
    out[n] = '\0';
    return out;
}

char* __cajeta_str_toLowerCase(const char* s) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    size_t n = strlen(s);
    char* out = (char*) malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char) (c + 32) : (char) c;
    }
    out[n] = '\0';
    return out;
}

// Strip ASCII whitespace from both ends. Mirrors Java's String.trim, which
// trims only U+0020 and below — not the broader Character.isWhitespace set.
char* __cajeta_str_trim(const char* s) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n && (unsigned char) s[i] <= 0x20) i++;
    size_t j = n;
    while (j > i && (unsigned char) s[j - 1] <= 0x20) j--;
    size_t len = j - i;
    char* out = (char*) malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s + i, len);
    out[len] = '\0';
    return out;
}

// Replace every occurrence of `from` in `s` with `to`. Returns malloc'd.
// If `from` is empty or null, the input is returned as a fresh copy.
char* __cajeta_str_replace(const char* s, const char* from, const char* to) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    size_t slen = strlen(s);
    if (!from || from[0] == '\0') {
        char* out = (char*) malloc(slen + 1);
        if (out) memcpy(out, s, slen + 1);
        return out;
    }
    if (!to) to = "";
    size_t flen = strlen(from);
    size_t tlen = strlen(to);
    // Count occurrences to size the output buffer.
    size_t count = 0;
    const char* p = s;
    while ((p = strstr(p, from)) != NULL) { count++; p += flen; }
    size_t outLen = slen + count * (tlen > flen ? (tlen - flen) : 0)
                         - count * (flen > tlen ? (flen - tlen) : 0);
    char* out = (char*) malloc(outLen + 1);
    if (!out) return NULL;
    char* w = out;
    p = s;
    while (1) {
        const char* m = strstr(p, from);
        if (!m) { strcpy(w, p); break; }
        size_t pre = (size_t) (m - p);
        memcpy(w, p, pre);
        w += pre;
        memcpy(w, to, tlen);
        w += tlen;
        p = m + flen;
    }
    return out;
}

// --- system / time / random helpers -----------------------------------------

#include <time.h>

// System.exit(code). Terminates the process — caller must not rely on return.
__attribute__((noreturn))
void __cajeta_exit(int32_t code) {
    // Use _Exit so atexit handlers (incl. stdio buffer flush) don't run; matches
    // Java's exit semantics where the JVM is torn down without C-style cleanup.
    _Exit(code);
}

// System.currentTimeMillis(). Wall-clock ms since the Unix epoch.
int64_t __cajeta_currentTimeMillis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t) ts.tv_sec * 1000 + (int64_t) (ts.tv_nsec / 1000000);
}

// Math.random() — pseudo-random double in [0.0, 1.0).
// Seeded lazily from the wall clock on first call so independent runs differ;
// concurrent callers race on the seed but the resulting numbers are still
// uniformly distributed in expectation, which is good enough for typical use.
double __cajeta_random(void) {
    static int seeded = 0;
    if (!seeded) {
        seeded = 1;
        srand((unsigned) __cajeta_currentTimeMillis());
    }
    return (double) rand() / ((double) RAND_MAX + 1.0);
}

// substring(begin, end) — like Java's String.substring, half-open. Out-of-range
// indices clamp to the valid window; the result is always a freshly malloc'd
// null-terminated copy (so callers can pass it back to concat etc.).
char* __cajeta_str_substring(const char* s, int64_t begin, int64_t end) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    int64_t n = (int64_t) strlen(s);
    if (begin < 0) begin = 0;
    if (end > n) end = n;
    if (end < begin) end = begin;
    int64_t len = end - begin;
    char* out = (char*) malloc((size_t) len + 1);
    if (!out) return NULL;
    memcpy(out, s + begin, (size_t) len);
    out[len] = '\0';
    return out;
}

// --- general-purpose hashing (cajeta.hash backend) --------------------------
// Implements the runtime hash primitives the language uses for Object.hash()
// and the cajeta.hash.* stdlib classes (see StandardLibrary.md §cajeta.hash
// and CajetaReflect.md "Performance"). Two algorithms cover the surface:
//
//   * SplitMix64 finalizer for primitive value hashing — int64.hash(),
//     int32.hash(), float64.hash(), float32.hash(), boolean.hash(),
//     pointer identity. Three multiplications + three XORs; well-tested
//     mixer (Java's SplittableRandom, Rust hashers, etc.). The per-process
//     seed is XOR'd in before mixing so two runs of the same program
//     produce different hash values (hash-flooding defense — attackers
//     can't predict bucket placement).
//
//   * XXH3-64 (scalar) for arbitrary byte buffers — backing for
//     cajeta.hash.XXHash3 and DefaultHasher. Multi-GB/s on modern CPUs.
//     Lands in a follow-up commit; this one ships the primitive-hash +
//     seed infrastructure first because HashMap<int64, V> and similar
//     primitive-keyed maps don't need it.
//
// The seed initializes once per process from /dev/urandom via a
// constructor function that fires before main(). Falls back to
// wall-clock + pid mixed through SplitMix64 if /dev/urandom isn't
// readable (sandboxes, embedded targets).

#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static uint64_t __cajeta_hash_seed_value = 0;

__attribute__((constructor))
static void __cajeta_hash_seed_init(void) {
    uint64_t s = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, &s, sizeof(s));
        close(fd);
        if (n == (ssize_t) sizeof(s) && s != 0) {
            __cajeta_hash_seed_value = s;
            return;
        }
    }
    // Fallback: wall clock + pid mixed through SplitMix64. Lower-entropy
    // than /dev/urandom but still per-process-distinct and stable for
    // the lifetime of the process.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t x = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
    x ^= (uint64_t) getpid() * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    __cajeta_hash_seed_value = x ? x : 0x9E3779B97F4A7C15ULL;
}

// Exposed to user code as cajeta.hash.Hash.processSeed() — useful when
// caller-side hashing needs to align with the synthesized Object.hash()
// values (e.g. external hash table snapshot replay).
int64_t __cajeta_hash_seed(void) {
    return (int64_t) __cajeta_hash_seed_value;
}

// SplitMix64 finalizer — the mixer behind every primitive hash variant.
// Three multiplications + three XOR-shifts; passes SMHasher avalanche
// + bias + collision tests on its own.
static inline uint64_t splitmix64_finalize(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

int64_t __cajeta_hash_int64(int64_t value) {
    return (int64_t) splitmix64_finalize((uint64_t) value ^ __cajeta_hash_seed_value);
}

int64_t __cajeta_hash_int32(int32_t value) {
    // Sign-extend so all-ones int32 doesn't hash like ~0 int64 just by
    // happening to share the low bits.
    return (int64_t) splitmix64_finalize(
        (uint64_t) (int64_t) value ^ __cajeta_hash_seed_value);
}

int64_t __cajeta_hash_float64(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    // Canonicalize -0 to +0 — IEEE 754 says +0 == -0, so they must hash
    // identically. NaN ordering is unspecified by the standard; we hash
    // each distinct NaN bit pattern to a distinct value, which is what
    // serializers / HashMap callers usually want.
    if (bits == 0x8000000000000000ULL) bits = 0;
    return (int64_t) splitmix64_finalize(bits ^ __cajeta_hash_seed_value);
}

int64_t __cajeta_hash_float32(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if (bits == 0x80000000U) bits = 0;
    return (int64_t) splitmix64_finalize((uint64_t) bits ^ __cajeta_hash_seed_value);
}

int64_t __cajeta_hash_boolean(int8_t value) {
    return (int64_t) splitmix64_finalize(
        (value ? 1ULL : 0ULL) ^ __cajeta_hash_seed_value);
}

// Pointer-identity hash. Used by IdentityHashMap, observer registries,
// weak-ref tables. Same mixer as the primitive variants so the
// distribution properties match.
int64_t __cajeta_hash_identity(void* p) {
    return (int64_t) splitmix64_finalize(
        (uint64_t)(uintptr_t) p ^ __cajeta_hash_seed_value);
}

// Combine two 64-bit hash values into one. Boost's hash_combine pattern
// adapted with the SplitMix mixer at the end. Used by manual hash()
// overrides that thread multiple field hashes together.
int64_t __cajeta_hash_combine(int64_t a, int64_t b) {
    uint64_t h = (uint64_t) a;
    h ^= (uint64_t) b + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return (int64_t) splitmix64_finalize(h);
}

// --- parsing helpers --------------------------------------------------------
// All return on error: 0 (for numeric forms) and false (for boolean). The
// stdlib spec for Cajeta will likely tighten this to a thrown exception once
// the exception ABI carries class info; for now zero-on-error keeps callers
// from needing a try/catch around routine parsing.

int64_t __cajeta_parse_i64(const char* s) {
    if (!s) return 0;
    return (int64_t) strtoll(s, NULL, 10);
}

double __cajeta_parse_f64(const char* s) {
    if (!s) return 0.0;
    return strtod(s, NULL);
}

int32_t __cajeta_parse_bool(const char* s) {
    if (!s) return 0;
    // Case-insensitive match for "true"; everything else is false (Java semantics).
    size_t n = strlen(s);
    if (n != 4) return 0;
    return (((s[0] | 0x20) == 't') && ((s[1] | 0x20) == 'r')
         && ((s[2] | 0x20) == 'u') && ((s[3] | 0x20) == 'e')) ? 1 : 0;
}

// String.valueOf(char) — wraps a single byte as a 1-char malloc'd string.
char* __cajeta_str_fromChar(int8_t c) {
    char* out = (char*) malloc(2);
    if (!out) return NULL;
    out[0] = (char) c;
    out[1] = '\0';
    return out;
}

char* __cajeta_str_concat(const char* a, const char* b) {
    if (!a) a = "null";
    if (!b) b = "null";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char* out = (char*) malloc(la + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

// SLF4J-style format: each `{}` in `fmt` is replaced in order by argv[i]'s
// null-terminated string. Extra args after all `{}`s are dropped. Missing
// args (more `{}`s than argv entries) print "null".
void __cajeta_log(int32_t stream, const char* fmt, int64_t argc, const char* const* argv) {
    if (!fmt) return;
    const char* p = fmt;
    int64_t argIdx = 0;
    while (*p) {
        if (p[0] == '{' && p[1] == '}') {
            const char* arg = (argv && argIdx < argc) ? argv[argIdx] : NULL;
            if (arg) {
                __cajeta_emit(stream, arg, strlen(arg));
            } else {
                __cajeta_emit(stream, "null", 4);
            }
            p += 2;
            argIdx++;
        } else {
            // Emit one run of literal text up to the next `{}` (or end).
            const char* run = p;
            while (*p && !(p[0] == '{' && p[1] == '}')) p++;
            __cajeta_emit(stream, run, (size_t) (p - run));
        }
    }
}

// ---------------------------------------------------------------------------
// At-exit registry — used by @PreDestroy synthesis (AspectModel.md § A11).
//
// The DI singleton @PreDestroy hook needs to fire at "process exit"
// semantics. libc's atexit() works for AOT binaries but dangles in
// JIT'd test runs: each test compiles a fresh module that's freed
// before the next test starts, so any function pointer registered
// from inside the JIT becomes invalid once that test's LLJIT state
// is destroyed. Routing through this runtime-internal registry lets
// the caller (a test, or main() in a real binary) explicitly fire
// handlers at a safe point and clear the list.
//
// Handlers run in LIFO order — the newest registration fires first.
// Each callback receives the instance pointer captured at register
// time; the user method must have signature `void (this:pointer)`
// (the ABI-compatible C type used here is `void (*)(void*)`).

typedef struct CajetaAtExitNode {
    void (*fn)(void*);
    void* arg;
    struct CajetaAtExitNode* next;
} CajetaAtExitNode;

static CajetaAtExitNode* __cajeta_atexit_head = NULL;

void __cajeta_atexit_push(void (*fn)(void*), void* arg) {
    if (!fn) return;
    CajetaAtExitNode* n = (CajetaAtExitNode*) malloc(sizeof(CajetaAtExitNode));
    if (!n) return;
    n->fn = fn;
    n->arg = arg;
    n->next = __cajeta_atexit_head;
    __cajeta_atexit_head = n;
}

void __cajeta_run_atexit_handlers(void) {
    CajetaAtExitNode* n = __cajeta_atexit_head;
    __cajeta_atexit_head = NULL;
    while (n) {
        CajetaAtExitNode* next = n->next;
        n->fn(n->arg);
        free(n);
        n = next;
    }
}
