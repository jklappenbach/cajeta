// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// --- Threading: scope frames ----------------------------------------------
// `scope { }` owns its child tasks; exit waits on every registered done-addr.

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

// --lazy-scope: push the implicit function-body frame on demand. Idempotent.
void __cajeta_scope_ensure_at(void* watermark) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    if ((void*) *top == watermark) {
        __cajeta_scope_enter();
    }
}

// Append a task's (done, exception) pair to the current frame; no-op outside one.
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
    f->entries[f->count].owned_task = NULL;
    f->count++;
}

// Register a DISCARDED spawn: the same join bookkeeping, plus the scope frees
// `task` afterwards — a per-site drop slot cannot hold N live tasks from a loop.
void __cajeta_scope_register_owned(int32_t* done_addr, void** exception_addr,
                                   void** fiber_slot, void* task) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    struct cajeta_scope_frame* f = *top;
    if (!f) return;
    __cajeta_scope_register(done_addr, exception_addr, fiber_slot);
    f->entries[f->count - 1].owned_task = task;
}

// Drop register entries pointing into this task, so an await re-throw leaves none.
void __cajeta_scope_deregister_task(void* task_ptr, uint64_t task_size) {
    if (!task_ptr) return;
    char* lo = (char*) task_ptr;
    char* hi = lo + task_size;
    for (struct cajeta_scope_frame* f = *__cajeta_scope_top_ptr();
            f; f = f->prev) {
        for (int i = 0; i < f->count; i++) {
            char* d = (char*) f->entries[i].done_addr;
            if (d >= lo && d < hi) {
                f->entries[i].done_addr = NULL;
                f->entries[i].exception_addr = NULL;
                f->entries[i].fiber_slot = NULL;
                f->entries[i].owned_task = NULL;
            }
        }
    }
}

// Wait on every registered done flag, then walk the exception slots and re-raise
// the first one found — "first-throw wins". Sibling cancellation is R5-C.
static void __cajeta_scope_release(struct cajeta_scope_frame* f) {
    // Free the tasks this scope owns; every caller has already joined them.
    for (int i = 0; i < f->count; i++) {
        if (f->entries[i].owned_task) {
            __cajeta_free(f->entries[i].owned_task);
        }
    }
    free(f->entries);
    free(f);
}

void __cajeta_scope_exit(void) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    struct cajeta_scope_frame* f = *top;
    if (!f) return;
    // First-throw wins: cancel the remaining children, then join them all anyway.
    void* trigger = NULL;
    for (int i = 0; i < f->count; i++) {
        if (!f->entries[i].done_addr) continue;   // deregistered (task freed)
        __cajeta_task_wait(f->entries[i].done_addr);
        if (!trigger && f->entries[i].exception_addr
                && *f->entries[i].exception_addr) {
            trigger = *f->entries[i].exception_addr;
            // Read each sibling's fiber slot and cancel under the task mutex.
            pthread_mutex_lock(&__cajeta_task_mutex);
            for (int j = i + 1; j < f->count; j++) {
                if (f->entries[j].fiber_slot && *f->entries[j].fiber_slot) {
                    __cajeta_fiber_cancel(
                        (struct cajeta_fiber*) *f->entries[j].fiber_slot,
                        trigger);
                }
            }
            pthread_mutex_unlock(&__cajeta_task_mutex);
        }
    }
    *top = f->prev;
    __cajeta_scope_release(f);
    if (trigger) {
        __cajeta_throw(trigger);
    }
}

// Watermark API for the implicit function-body scope: codegen captures scope_top
// at entry and calls this on every return, popping to it in LIFO order.
void* __cajeta_scope_save_top(void) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    return (void*) *top;
}

void __cajeta_scope_exit_to(void* watermark) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    // Innermost-out, so a topmost frame's throw wins over an outer frame's.
    void* trigger = NULL;
    while (*top != (struct cajeta_scope_frame*) watermark) {
        struct cajeta_scope_frame* f = *top;
        if (!f) break;
        void* frame_trigger = NULL;
        for (int i = 0; i < f->count; i++) {
            if (!f->entries[i].done_addr) continue;   // deregistered (task freed)
            __cajeta_task_wait(f->entries[i].done_addr);
            if (!frame_trigger && f->entries[i].exception_addr
                    && *f->entries[i].exception_addr) {
                frame_trigger = *f->entries[i].exception_addr;
                // C2: same mutex discipline as scope_exit's cancel loop.
                pthread_mutex_lock(&__cajeta_task_mutex);
                for (int j = i + 1; j < f->count; j++) {
                    if (f->entries[j].fiber_slot && *f->entries[j].fiber_slot) {
                        __cajeta_fiber_cancel(
                            (struct cajeta_fiber*) *f->entries[j].fiber_slot,
                            frame_trigger);
                    }
                }
                pthread_mutex_unlock(&__cajeta_task_mutex);
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
// A pthread_mutex_t for the lock's own metadata, a queue of parked fibers, and a
// condvar for a main-thread waiter. `held` is the flag user code contends on.

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
        // Main thread, or any non-fiber pthread: cond_wait for the release.
        pthread_mutex_lock(&l->mutex);
        while (l->held) {
            pthread_cond_wait(&l->released_cond, &l->mutex);
        }
        l->held = 1;
        pthread_mutex_unlock(&l->mutex);
        return;
    }
    // Fiber path: park if held, and on wake re-check rather than assume the lock
    // is ours — another acquirer may have raced in.
    for (;;) {
        pthread_mutex_lock(&l->mutex);
        struct cajeta_fiber* self = __cajeta_current_fiber;
        // Honor a cancellation delivered while parked, but pass a handoff on
        // first so the next waiter is not stranded.
        if (self->cancel_with) {
            void* cw = self->cancel_with;
            self->cancel_with = NULL;
            struct cajeta_fiber* nxt = NULL;
            if (!l->held) {
                nxt = l->wait_head;
                if (nxt) {
                    l->wait_head = nxt->next;
                    if (!l->wait_head) l->wait_tail = NULL;
                    nxt->next = NULL;
                }
            }
            pthread_mutex_unlock(&l->mutex);
            if (nxt) {
                // publish_ready does its own locking, so it must be called
                // OUTSIDE __cajeta_task_mutex.
                __cajeta_publish_ready(nxt);
            }
            __cajeta_throw(cw);
        }
        if (!l->held) {
            l->held = 1;
            pthread_mutex_unlock(&l->mutex);
            return;
        }
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
        __cajeta_swapcontext(&self->ctx, &__cajeta_carrier_ctx);
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
    // Signal a main-thread waiter even during a fiber handoff: no missed wakes.
    pthread_cond_signal(&l->released_cond);
    pthread_mutex_unlock(&l->mutex);
    if (next) {
        // Publish the woken fiber onto a carrier's deque (current, else 0).
        __cajeta_publish_ready(next);
    }
}

// Returns 1 if acquired, 0 if already held. Never parks, even on a fiber.
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
    // Destroying a held or waited-on lock is UB and would UAF a later release,
    // so refuse and leak: a destroy-while-busy is a program bug.
    pthread_mutex_lock(&l->mutex);
    int busy = l->held || l->wait_head != NULL;
    pthread_mutex_unlock(&l->mutex);
    if (busy) {
        fprintf(stderr, "cajeta: Lock destroyed while held or with waiters; "
                "leaking it to avoid undefined behavior\n");
        return;
    }
    if (pthread_cond_destroy(&l->released_cond) != 0 ||
        pthread_mutex_destroy(&l->mutex) != 0) {
        fprintf(stderr, "cajeta: Lock primitive still busy at destroy; leaked\n");
        return;
    }
    free(l);
}

// --- Threading sync primitives: condition variable (R7-B) ----------------
// Fiber-aware condvar paired with a Lock handle. One condvar, notify-all, and a
// per-waiter predicate re-check, so a spurious wake is harmless.

struct cajeta_async_condvar {
    pthread_mutex_t mutex;        // protects the wait queue; serializes with notify
    pthread_cond_t  main_cond;    // main-thread waiters block here
    struct cajeta_fiber* wait_head;
    struct cajeta_fiber* wait_tail;
};

void* __cajeta_condvar_new(void) {
    struct cajeta_async_condvar* cv =
        (struct cajeta_async_condvar*) malloc(sizeof(*cv));
    if (!cv) {
        fprintf(stderr, "cajeta: __cajeta_condvar_new failed\n");
        abort();
    }
    if (pthread_mutex_init(&cv->mutex, NULL) != 0) {
        fprintf(stderr, "cajeta: condvar pthread_mutex_init failed\n");
        free(cv);
        abort();
    }
    if (pthread_cond_init(&cv->main_cond, NULL) != 0) {
        fprintf(stderr, "cajeta: condvar pthread_cond_init failed\n");
        pthread_mutex_destroy(&cv->mutex);
        free(cv);
        abort();
    }
    cv->wait_head = NULL;
    cv->wait_tail = NULL;
    return cv;
}

// Atomically release `lockp`, suspend until notified, then reacquire it. The
// caller MUST hold `lockp` on entry and holds it again on return.
void __cajeta_condvar_wait(void* cvp, void* lockp) {
    if (!cvp || !lockp) return;
    struct cajeta_async_condvar* cv = (struct cajeta_async_condvar*) cvp;
    if (__cajeta_current_fiber) {
        // Enqueue under cv->mutex BEFORE releasing the user lock, or a
        // concurrent notify_all slips between the two and the wakeup is lost.
        struct cajeta_fiber* self = __cajeta_current_fiber;
        pthread_mutex_lock(&cv->mutex);
        self->next = NULL;
        if (cv->wait_tail) {
            cv->wait_tail->next = self;
            cv->wait_tail = self;
        } else {
            cv->wait_head = self;
            cv->wait_tail = self;
        }
        self->state = CAJETA_FIBER_PARKED;
        pthread_mutex_unlock(&cv->mutex);
        __cajeta_lock_release(lockp);
        __cajeta_swapcontext(&self->ctx, &__cajeta_carrier_ctx);
        // Resumed by notify_all: reacquire the user lock, which may itself park.
        __cajeta_lock_acquire(lockp);
        return;
    }
    // Main-thread path: cond_wait under cv->mutex, pairing with the broadcast.
    pthread_mutex_lock(&cv->mutex);
    __cajeta_lock_release(lockp);
    pthread_cond_wait(&cv->main_cond, &cv->mutex);
    pthread_mutex_unlock(&cv->mutex);
    __cajeta_lock_acquire(lockp);
}

// Wake every waiter — fibers onto the ready queue, the main thread by broadcast.
void __cajeta_condvar_notify_all(void* cvp) {
    if (!cvp) return;
    struct cajeta_async_condvar* cv = (struct cajeta_async_condvar*) cvp;
    pthread_mutex_lock(&cv->mutex);
    struct cajeta_fiber* w = cv->wait_head;
    cv->wait_head = NULL;
    cv->wait_tail = NULL;
    pthread_cond_broadcast(&cv->main_cond);
    pthread_mutex_unlock(&cv->mutex);
    while (w) {
        struct cajeta_fiber* next = w->next;
        __cajeta_publish_ready(w);
        w = next;
    }
}

void __cajeta_condvar_destroy(void* cvp) {
    if (!cvp) return;
    struct cajeta_async_condvar* cv = (struct cajeta_async_condvar*) cvp;
    pthread_cond_destroy(&cv->main_cond);
    pthread_mutex_destroy(&cv->mutex);
    free(cv);
}

// --- Threading sync primitives: reader-writer lock (R7-D) ----------------
// Many readers, one writer, writer-preference: a reader blocks while a writer
// waits. Unlock wakes ALL waiters, writers first, and each re-checks.

// Publish a NULL-terminated fiber list onto the carrier pool's deques.
static void __cajeta_ready_enqueue_list(struct cajeta_fiber* head) {
    while (head) {
        struct cajeta_fiber* next = head->next;
        __cajeta_publish_ready(head);
        head = next;
    }
}

struct cajeta_async_rwlock {
    pthread_mutex_t mutex;        // protects state + wait queues
    pthread_cond_t  main_cond;    // main-thread readers + writers block here
    int readers;                  // active shared-read holders
    int writer;                   // 1 if a writer holds it exclusively
    int writers_waiting;          // queued writers (drives writer-preference)
    struct cajeta_fiber* rwait_head;   // fiber readers waiting
    struct cajeta_fiber* rwait_tail;
    struct cajeta_fiber* wwait_head;   // fiber writers waiting
    struct cajeta_fiber* wwait_tail;
};

void* __cajeta_rwlock_new(void) {
    struct cajeta_async_rwlock* rw =
        (struct cajeta_async_rwlock*) malloc(sizeof(*rw));
    if (!rw) {
        fprintf(stderr, "cajeta: __cajeta_rwlock_new failed\n");
        abort();
    }
    if (pthread_mutex_init(&rw->mutex, NULL) != 0) {
        fprintf(stderr, "cajeta: rwlock pthread_mutex_init failed\n");
        free(rw);
        abort();
    }
    if (pthread_cond_init(&rw->main_cond, NULL) != 0) {
        fprintf(stderr, "cajeta: rwlock pthread_cond_init failed\n");
        pthread_mutex_destroy(&rw->mutex);
        free(rw);
        abort();
    }
    rw->readers = 0;
    rw->writer = 0;
    rw->writers_waiting = 0;
    rw->rwait_head = NULL;
    rw->rwait_tail = NULL;
    rw->wwait_head = NULL;
    rw->wwait_tail = NULL;
    return rw;
}

void __cajeta_rwlock_rdlock(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    if (!__cajeta_current_fiber) {
        pthread_mutex_lock(&rw->mutex);
        while (rw->writer || rw->writers_waiting > 0) {
            pthread_cond_wait(&rw->main_cond, &rw->mutex);
        }
        rw->readers++;
        pthread_mutex_unlock(&rw->mutex);
        return;
    }
    struct cajeta_fiber* self = __cajeta_current_fiber;
    pthread_mutex_lock(&rw->mutex);
    for (;;) {
        if (!rw->writer && rw->writers_waiting == 0) {
            rw->readers++;
            pthread_mutex_unlock(&rw->mutex);
            return;
        }
        self->next = NULL;
        if (rw->rwait_tail) {
            rw->rwait_tail->next = self;
            rw->rwait_tail = self;
        } else {
            rw->rwait_head = self;
            rw->rwait_tail = self;
        }
        self->state = CAJETA_FIBER_PARKED;
        pthread_mutex_unlock(&rw->mutex);
        __cajeta_swapcontext(&self->ctx, &__cajeta_carrier_ctx);
        pthread_mutex_lock(&rw->mutex);
    }
}

void __cajeta_rwlock_wrlock(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    if (!__cajeta_current_fiber) {
        pthread_mutex_lock(&rw->mutex);
        rw->writers_waiting++;
        while (rw->writer || rw->readers > 0) {
            pthread_cond_wait(&rw->main_cond, &rw->mutex);
        }
        rw->writers_waiting--;
        rw->writer = 1;
        pthread_mutex_unlock(&rw->mutex);
        return;
    }
    struct cajeta_fiber* self = __cajeta_current_fiber;
    pthread_mutex_lock(&rw->mutex);
    rw->writers_waiting++;
    for (;;) {
        if (!rw->writer && rw->readers == 0) {
            rw->writers_waiting--;
            rw->writer = 1;
            pthread_mutex_unlock(&rw->mutex);
            return;
        }
        self->next = NULL;
        if (rw->wwait_tail) {
            rw->wwait_tail->next = self;
            rw->wwait_tail = self;
        } else {
            rw->wwait_head = self;
            rw->wwait_tail = self;
        }
        self->state = CAJETA_FIBER_PARKED;
        pthread_mutex_unlock(&rw->mutex);
        __cajeta_swapcontext(&self->ctx, &__cajeta_carrier_ctx);
        pthread_mutex_lock(&rw->mutex);
    }
}

// Detach both wait queues (writers first) and wake them once unlocked.
static void __cajeta_rwlock_wake_all_locked(struct cajeta_async_rwlock* rw,
                                            struct cajeta_fiber** ww,
                                            struct cajeta_fiber** rwq) {
    *ww = rw->wwait_head;
    rw->wwait_head = NULL;
    rw->wwait_tail = NULL;
    *rwq = rw->rwait_head;
    rw->rwait_head = NULL;
    rw->rwait_tail = NULL;
    pthread_cond_broadcast(&rw->main_cond);
}

void __cajeta_rwlock_rdunlock(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    struct cajeta_fiber* ww = NULL;
    struct cajeta_fiber* rwq = NULL;
    pthread_mutex_lock(&rw->mutex);
    if (rw->readers > 0) rw->readers--;
    // Only the last reader out can let a writer in; wake then.
    if (rw->readers == 0) {
        __cajeta_rwlock_wake_all_locked(rw, &ww, &rwq);
    }
    pthread_mutex_unlock(&rw->mutex);
    __cajeta_ready_enqueue_list(ww);
    __cajeta_ready_enqueue_list(rwq);
}

void __cajeta_rwlock_wrunlock(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    struct cajeta_fiber* ww = NULL;
    struct cajeta_fiber* rwq = NULL;
    pthread_mutex_lock(&rw->mutex);
    rw->writer = 0;
    __cajeta_rwlock_wake_all_locked(rw, &ww, &rwq);
    pthread_mutex_unlock(&rw->mutex);
    __cajeta_ready_enqueue_list(ww);
    __cajeta_ready_enqueue_list(rwq);
}

void __cajeta_rwlock_destroy(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    pthread_cond_destroy(&rw->main_cond);
    pthread_mutex_destroy(&rw->mutex);
    free(rw);
}

// --- atomic<T> backing storage (R8 Slice 1) ---------------------------------
// Atomic<T> owns a heap word that compiler-emitted inline atomics act on; the
// runtime only allocs and frees it, so ordering stays in IR where it optimizes.
int32_t* __cajeta_atomic_i32_new(int32_t initial) {
    int32_t* cell = (int32_t*) malloc(sizeof(int32_t));
    if (!cell) {
        fprintf(stderr, "cajeta: __cajeta_atomic_i32_new failed\n");
        abort();
    }
    // The initial store is seq_cst: another carrier sees the constructed value.
    __atomic_store_n(cell, initial, __ATOMIC_SEQ_CST);
    return cell;
}

void __cajeta_atomic_i32_destroy(int32_t* cell) {
    if (cell) free(cell);
}

int64_t* __cajeta_atomic_i64_new(int64_t initial) {
    int64_t* cell = (int64_t*) malloc(sizeof(int64_t));
    if (!cell) {
        fprintf(stderr, "cajeta: __cajeta_atomic_i64_new failed\n");
        abort();
    }
    __atomic_store_n(cell, initial, __ATOMIC_SEQ_CST);
    return cell;
}

void __cajeta_atomic_i64_destroy(int64_t* cell) {
    if (cell) free(cell);
}

// Abort with a diagnostic when an array index is out of bounds; emitted as a
// conditional branch from ArrayIndexExpression when bounds checking is on.
void __cajeta_array_bounds_fail(int64_t index, int64_t dim) {
    // write(2), not fprintf: abort() does not flush stdio and a piped Windows
    // stderr is block-buffered, so the message must reach the fd directly.
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "cajeta: array index %lld out of bounds for dimension size %lld\n",
                     (long long) index, (long long) dim);
    if (n > 0) {
        if (n > (int) sizeof(buf)) n = (int) sizeof(buf);
        (void) write(2, buf, (size_t) n);
    }
    abort();
}

// --- exception handling (setjmp/longjmp-based) -------------------------------
// A try-block registers a stack frame; `throw` writes into the topmost and
// longjmps to its setjmp point. Each OS thread and each fiber owns its own top.

// --- drop chain (Session 3 of the memory-model rollout) ---------------------
// Owners push an entry at declaration and pop+drop at scope exit; the throw path
// walks the chain down to the catching frame's watermark.

struct cajeta_drop_entry {
    void* obj;
    void (*drop_fn)(void*);
    struct cajeta_drop_entry* prev;
    int8_t active;  // i8 instead of bool — fixed ABI for the IR side
};

// Debug-mode variant: the base entry plus source-position tags. Its first four
// fields share the base layout, so pop_run needs no separate helper.
struct cajeta_drop_entry_debug {
    void* obj;                                  // +0
    void (*drop_fn)(void*);                     // +8
    struct cajeta_drop_entry* prev;             // +16  (same shape as base)
    int8_t active;                              // +24
    int8_t _pad[3];                             // +25
    int32_t alloc_line;                         // +28
    const char* alloc_file;                     // +32
    /* total: 40 bytes */
};

size_t __cajeta_drop_entry_size(void) {
    return sizeof(struct cajeta_drop_entry);
}

size_t __cajeta_drop_entry_size_debug(void) {
    return sizeof(struct cajeta_drop_entry_debug);
}

// Drop chain head — per-thread, since a carrier concurrent with main would race.
static __thread struct cajeta_drop_entry* __cajeta_main_drop_top = NULL;

// The current drop_top slot, the running fiber's or the main thread's, so
// push/pop works in either context. Mirrors __cajeta_scope_top_ptr.
static struct cajeta_drop_entry** __cajeta_drop_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->drop_top;
    }
    return &__cajeta_main_drop_top;
}

// --- FiberLocal: ambient per-request state -----------------------------------
// A fiber-keyed, scope-restored binding stack. Fibers are single-use, so a
// binding cannot leak into a later request the way a pooled __thread slot would.

struct cajeta_fiber_local {
    void* key;                        // FiberLocal<T> instance identity
    void* value;                      // T (reference payload)
    struct cajeta_fiber_local* prev;  // next-oldest binding
};

// Main-thread binding head; a carrier-hosted fiber uses its own fl_top.
static __thread struct cajeta_fiber_local* __cajeta_main_fl_top = NULL;

static struct cajeta_fiber_local** __cajeta_fl_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->fl_top;
    }
    return &__cajeta_main_fl_top;
}

// Deep-copy a binding chain onto `base` (NULL for a standalone snapshot).
static struct cajeta_fiber_local* __cajeta_fiber_local_copy_onto(
        struct cajeta_fiber_local* head, struct cajeta_fiber_local* base) {
    if (!head) return base;
    struct cajeta_fiber_local* prev_copy =
        __cajeta_fiber_local_copy_onto(head->prev, base);
    struct cajeta_fiber_local* c = malloc(sizeof(*c));
    if (!c) {
        fprintf(stderr, "cajeta: fiber-local copy malloc failed\n");
        abort();
    }
    c->key = head->key;
    c->value = head->value;
    c->prev = prev_copy;
    return c;
}

// Free a chain newest-first. NULL-safe.
static void __cajeta_fiber_local_free_chain(struct cajeta_fiber_local* head) {
    while (head) {
        struct cajeta_fiber_local* prev = head->prev;
        free(head);
        head = prev;
    }
}

// Snapshot the spawner's chain as a deep copy, for inherit-on-spawn.
static struct cajeta_fiber_local* __cajeta_fiber_local_snapshot_current(void) {
    return __cajeta_fiber_local_copy_onto(*__cajeta_fl_top_ptr(), NULL);
}

// --- intrinsics (wrapped by cajeta.concurrent.FiberLocal / FiberContext) -----

// Push a binding; returns the prior head as an opaque restore token.
void* __cajeta_fiber_local_push(void* key, void* value) {
    struct cajeta_fiber_local** head = __cajeta_fl_top_ptr();
    struct cajeta_fiber_local* f = malloc(sizeof(*f));
    if (!f) {
        fprintf(stderr, "cajeta: fiber-local push malloc failed\n");
        abort();
    }
    f->key = key;
    f->value = value;
    f->prev = *head;
    void* token = (void*) *head;
    *head = f;
    return token;
}

// Restore the head to `token`, freeing every frame newer than it.
void __cajeta_fiber_local_pop(void* token) {
    struct cajeta_fiber_local** head = __cajeta_fl_top_ptr();
    struct cajeta_fiber_local* restore = (struct cajeta_fiber_local*) token;
    struct cajeta_fiber_local* cur = *head;
    while (cur != restore) {
        struct cajeta_fiber_local* prev = cur->prev;
        free(cur);
        cur = prev;
    }
    *head = restore;
}

// Current binding for `key`, newest-first; NULL when unbound.
void* __cajeta_fiber_local_get(void* key) {
    for (struct cajeta_fiber_local* c = *__cajeta_fl_top_ptr(); c; c = c->prev) {
        if (c->key == key) return c->value;
    }
    return NULL;
}

int32_t __cajeta_fiber_local_is_bound(void* key) {
    for (struct cajeta_fiber_local* c = *__cajeta_fl_top_ptr(); c; c = c->prev) {
        if (c->key == key) return 1;
    }
    return 0;
}

// FiberContext: an immutable snapshot of all bindings, for handoff across an
// unstructured boundary. install() returns a token to pop back to.
void* __cajeta_fiber_context_capture(void) {
    return (void*) __cajeta_fiber_local_copy_onto(*__cajeta_fl_top_ptr(), NULL);
}

void* __cajeta_fiber_context_install(void* snapshot) {
    struct cajeta_fiber_local** head = __cajeta_fl_top_ptr();
    void* token = (void*) *head;
    *head = __cajeta_fiber_local_copy_onto(
        (struct cajeta_fiber_local*) snapshot, *head);
    return token;
}

void __cajeta_fiber_context_free(void* snapshot) {
    __cajeta_fiber_local_free_chain((struct cajeta_fiber_local*) snapshot);
}

// Observability for tests: bumped whenever a drop function actually fires.
int64_t __cajeta_drop_count = 0;

int64_t __cajeta_drop_count_get(void) {
    return __atomic_load_n(&__cajeta_drop_count, __ATOMIC_SEQ_CST);
}
void __cajeta_drop_count_reset(void) {
    __atomic_store_n(&__cajeta_drop_count, 0, __ATOMIC_SEQ_CST);
}
// Add a batch of drops at once, for the frame-arena reset's bump-reclaim.
void __cajeta_drop_count_add(int64_t n) {
    __atomic_fetch_add(&__cajeta_drop_count, n, __ATOMIC_SEQ_CST);
}

// ----------------------------------------------------------------------------
