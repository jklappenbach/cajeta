// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
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

// --lazy-scope: push the implicit function-body scope frame on demand. With
// --lazy-scope the prologue skips the unconditional scope_enter (it heap-allocs
// on every call but is only needed for a bare `spawn`); a bare spawn calls this
// before registering so it still has a frame to join against. Idempotent within
// a method body: once a frame is pushed *top != watermark and this no-ops, and
// an enclosing `scope { }` (its own frame) likewise makes it a no-op. The frame
// is waited + popped by __cajeta_scope_exit_to(watermark) on every return path.
void __cajeta_scope_ensure_at(void* watermark) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    if ((void*) *top == watermark) {
        __cajeta_scope_enter();
    }
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

// Remove any scope_register entries whose done_addr falls inside the given
// task struct. Called by the task drop function so a re-throw via await
// (which unwinds the drop chain and frees the task before scope_exit_to
// runs) doesn't leave scope_exit_to dereferencing freed memory. The
// freed-task range used to be exception_addr (one slot inside the task) —
// matching any field-of-task pointer is the same call.
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
            }
        }
    }
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
            // C2: read each sibling's fiber slot + cancel under the task mutex so
            // it can't race the carrier nulling/freeing a completed fiber.
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
        struct cajeta_fiber* self = __cajeta_current_fiber;
        // H7: honor a cancellation delivered while parked on this lock — throw
        // instead of acquiring and running the critical section uncancelled. If we
        // were just handed the lock (held==0, i.e. a release woke us rather than
        // another acquirer racing in), pass the handoff to the next waiter first
        // so it isn't stranded.
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
                // Wake the handed-off waiter through the work-stealing scheduler:
                // publish_ready routes the (already-started) fiber to its home
                // carrier's deque and signals a sleeper. It does its own locking,
                // so it must be called OUTSIDE __cajeta_task_mutex.
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
    // Signal one main-thread waiter even when a fiber is being handed
    // the lock — costs almost nothing and avoids missed-wake races with
    // a pthread that happens to be cond_waiting concurrently.
    pthread_cond_signal(&l->released_cond);
    pthread_mutex_unlock(&l->mutex);
    if (next) {
        // Publish the woken fiber onto a carrier's deque (current carrier
        // if running on one, else carrier 0). publish_ready takes the
        // per-carrier deque_mutex + signals a sleeping carrier through
        // the pool condvar.
        __cajeta_publish_ready(next);
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
    // H8: destroying a lock that is still held or has parked waiters is UB
    // (pthread_*_destroy on a busy primitive), strands the waiting fibers, and
    // UAFs any in-flight/subsequent release that dereferences `l`. Refuse and leak
    // it rather than corrupt — a destroy-while-busy is a program bug.
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
//
// Fiber-aware condvar, paired with a Lock handle. `Mutex<T>.withLockWhen`
// (docs/specification/concurrent/Concurrency.md § Mutex) builds on it: wait-for-predicate
// inside a held lock, with notify on every mutation. A single condvar with
// notify-all + per-waiter predicate re-check is the v1 strategy (spurious
// wakeups are possible but harmless — each waiter re-tests its own
// condition).
//
// Same single-carrier discipline as the Lock: a fiber waiter enqueues on
// the condvar's own wait queue and swaps to the carrier; a main-thread
// waiter blocks on the condvar's pthread cond. notify_all moves every fiber
// waiter onto the carrier ready queue and broadcasts the pthread cond.

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

// Atomically release `lockp`, suspend until notified, then reacquire `lockp`.
// The caller MUST hold `lockp` on entry; it holds it again on return.
void __cajeta_condvar_wait(void* cvp, void* lockp) {
    if (!cvp || !lockp) return;
    struct cajeta_async_condvar* cv = (struct cajeta_async_condvar*) cvp;
    if (__cajeta_current_fiber) {
        // Fiber path: enqueue self on the condvar wait queue, release the
        // user lock, then swap to the carrier. Enqueue happens under
        // cv->mutex BEFORE the lock release so a concurrent notify_all
        // (which also takes cv->mutex) can't slip between enqueue and
        // release and lose the wakeup — once we're on the queue, notify
        // will move us to the ready queue and we'll be resumed.
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
        // Resumed by notify_all (carrier re-dispatched us). Reacquire the
        // user lock before returning — may itself park if the lock is held.
        __cajeta_lock_acquire(lockp);
        return;
    }
    // Main-thread path: classic cond_wait. Release the user lock, block on
    // the condvar's own pthread cond, reacquire on wake. cv->mutex guards
    // the wait so it pairs with notify_all's broadcast under the same mutex.
    pthread_mutex_lock(&cv->mutex);
    __cajeta_lock_release(lockp);
    pthread_cond_wait(&cv->main_cond, &cv->mutex);
    pthread_mutex_unlock(&cv->mutex);
    __cajeta_lock_acquire(lockp);
}

// Wake every waiter: move all fiber waiters onto the carrier ready queue and
// broadcast the main-thread cond. Woken waiters re-check their predicate.
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
//
// Fiber-aware RW lock backing `RwLock<T>` (docs/specification/concurrent/Concurrency.md §
// RwLock). Many readers may hold it concurrently; a writer holds it
// exclusively. Writer-preference: a reader blocks while any writer is
// waiting, so a steady stream of readers can't starve a writer.
//
// Same single-carrier discipline as Lock/condvar: a blocked fiber parks on
// the appropriate wait queue and swaps to the carrier; a blocked main
// thread cond_waits. Unlock wakes ALL waiters (writers first) and lets each
// re-check its predicate — spurious wakeups are harmless, matching the
// condvar's notify-all strategy. Under the single carrier this costs little.

// Publish a NULL-terminated fiber list onto the carrier pool's deques.
// Each fiber is routed via __cajeta_publish_ready, which picks the
// current carrier (if any) or carrier 0 and takes that carrier's
// deque_mutex. Shared by the rwlock unlock paths.
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

// Detach both wait queues (writers first) and wake them once unlocked, so
// woken fibers re-check their predicate against the new state.
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
//
// Atomic<T> classes (cajeta.concurrent.AtomicInt32 / AtomicInt64) own a heap-
// allocated word that the compiler-emitted inline LLVM atomic instructions
// (atomicrmw / cmpxchg / load atomic / store atomic) operate on directly. The
// runtime's role is just the alloc/free of the underlying cell — the
// arithmetic and ordering live in IR so the optimizer can reason about them
// (a runtime call would defeat the point of an atomic). Plain malloc/free is
// fine: these cells aren't tracked in the live-set (the owning Atomic<T>
// class's drop wrapper calls the destroy intrinsic, and the cell never
// outlives its owner).
int32_t* __cajeta_atomic_i32_new(int32_t initial) {
    int32_t* cell = (int32_t*) malloc(sizeof(int32_t));
    if (!cell) {
        fprintf(stderr, "cajeta: __cajeta_atomic_i32_new failed\n");
        abort();
    }
    // Initial store is seq_cst so a subsequent reader on another carrier
    // sees the constructed value without an extra fence at the call site.
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

// Abort with a diagnostic when an array index is out of bounds. Compiler emits a
// conditional branch to this from ArrayIndexExpression when bounds checking is on.
void __cajeta_array_bounds_fail(int64_t index, int64_t dim) {
    // write(2) rather than fprintf(stderr): abort() does not flush stdio, and
    // on Windows stderr is block-buffered when piped (e.g. under a gtest death
    // test), so an fprintf'd message is lost — the diagnostic must reach the
    // fd directly. snprintf into a stack buffer first (no FILE* involved).
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

// Debug-mode variant — extends the base entry with source-position
// tags (docs/CompilerModes.md § Source-tagged drop-chain
// entries). The first four fields share the base entry's layout, so
// a debug entry passed to pop_run (which only touches obj/drop_fn/
// prev/active) works without a separate pop helper.
//
// The compiler emits debug entries when CompilerFlags::sourceTags is
// on, allocating CAJETA_DROP_ENTRY_BYTES_DEBUG (40) instead of the
// release-mode 32, and calls __cajeta_drop_push_debug to populate
// the tag fields. SIGABRT handler / diagnostic dumps read the tags
// via the helpers below.
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

// --- FiberLocal: ambient per-request state (docs/specification/concurrent/FiberLocal.md) -------
//
// A fiber-keyed, scope-restored binding stack — the sound replacement for a
// thread-pool ThreadLocal (fibers are single-use, so a binding cannot leak into
// a later unrelated request; carriers are pooled, so a __thread slot would alias
// across the fibers a carrier hosts — hence per-fiber storage, like the drop /
// exception / scope chains above). Bindings are keyed by the FiberLocal<T>
// object's identity (an opaque `void* key`); values are reference-typed (a
// `void*`), which is why the surface restricts T to a reference type in v1.
//
// Frames are immutable once linked; a push prepends, a pop unlinks+frees newest-
// first down to a restore token. where()/FiberContext.run() bracket push/pop via
// the language's try/finally so cleanup fires on the normal AND the throw edge.

struct cajeta_fiber_local {
    void* key;                        // FiberLocal<T> instance identity
    void* value;                      // T (reference payload)
    struct cajeta_fiber_local* prev;  // next-oldest binding
};

// Main-thread binding head — mirrors __cajeta_main_drop_top. A carrier-hosted
// fiber uses its own cajeta_fiber.fl_top; __cajeta_fl_top_ptr picks between them.
static __thread struct cajeta_fiber_local* __cajeta_main_fl_top = NULL;

static struct cajeta_fiber_local** __cajeta_fl_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->fl_top;
    }
    return &__cajeta_main_fl_top;
}

// Deep-copy a binding chain, linking the deepest copied frame onto `base`
// (NULL for a standalone snapshot). Recursion depth == binding count, which is
// tiny (a handful per request); newest-first order is preserved.
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

// Snapshot the CURRENT (spawner's) chain as an independent deep copy — used by
// __cajeta_task_run for inherit-on-spawn.
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

// Current binding for `key`, newest-first; NULL when unbound (the surface uses
// __cajeta_fiber_local_is_bound to distinguish "bound to null" from "unbound").
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

// FiberContext: an immutable snapshot of all current bindings, for explicit
// handoff across an unstructured boundary (channel / detach). capture() deep-
// copies the chain; install() layers a fresh copy on top of the receiving
// fiber's head and returns a token to pop back to; free() releases the snapshot
// when the FiberContext object drops.
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
// Add a batch of drops at once — the frame-arena reset (cajeta_rt_core.c) uses this
// to account for the non-escaping primitive arrays it reclaims in one bump-reset,
// restoring the pre-frame-arena per-array drop tick without the per-array free.
void __cajeta_drop_count_add(int64_t n) {
    __atomic_fetch_add(&__cajeta_drop_count, n, __ATOMIC_SEQ_CST);
}

// ----------------------------------------------------------------------------
