# Cajeta Concurrency Model — Specification v1

## Goals

- **No data races at compile time.** The ownership/borrow machinery already used for single-threaded memory safety extends to thread boundaries; sharing mutable state without serialization is rejected before codegen.
- **Structured concurrency by default.** Every spawned task has a scope. Scopes don't return until their children complete or are cancelled. "Fire and forget" requires explicit opt-in.
- **No SAM interfaces for tasks.** Threading primitives consume function-typed values — there is no `Runnable`, no `Callable`, no `Future.get()` vs `CompletableFuture.then*` schism.
- **No `synchronized`, no `volatile`, no `ThreadLocal`.** Each of those Java warts comes from sharing mutable state across threads; Cajeta makes that the unusual case, not the default. Locks exist, but only via the RAII-guard primitives below — never as a separate "acquire / forget to release" API. (For the *legitimate* use of `ThreadLocal` — ambient per-request state — the sound, fiber-keyed replacement is **`FiberLocal`**; see [`FiberLocal.md`](FiberLocal.md).)
- **No GC, no runtime bootstrap.** Task state machines lower to plain structs allocated on the single-owner heap. The runtime is a small set of C functions (executor + scheduler + sync primitives), in the same style as the existing `__cajeta_*` helpers.

## Non-goals (v1)

- Preemptive cancellation. Cancellation is cooperative — a task observes a cancel signal at its next `await`.
- Custom executors beyond the built-in scheduler. (The executor is a carrier pool with per-carrier work-stealing deques, sized via `CAJETA_CARRIERS`, multi-carrier by default (`min(nproc, 4)`) — see the scheduler note under `async fn`; replacing it wholesale is not in scope.)
- Distributed actors (RPC across machines).
- Effect handlers / algebraic effects.
- Async iteration / streaming (`AsyncIterator` style); covered separately if added.

---

## Core primitives

### 1. `async fn` — suspendable functions

```
async int32 fetchScore(Url u) {
    Response r = await http.get(u);
    return r.statusCode;
}
```

Every `async` function has return type `Task<T>`. Awaiting a `Task<T>` either:
- returns the inner `T` immediately if the task has completed, or
- suspends the current task and resumes when the awaited task completes.

`await` is only legal inside `async fn` / inside a `spawn` body. Calling an `async fn` without `await` produces an unstarted `Task<T>` — it does nothing until awaited or `spawn`ed.

Under the hood: each task is a **stackful fiber** — its own heap-allocated stack (~64 KB initial) plus a saved `ucontext`. `await` parks the running fiber and switches back to its carrier, which picks another ready fiber off its deque. No `async fn` codegen transformation is required — fibers' suspension lives in the C runtime's context-switch primitive, and from the compiler's perspective an `async fn` is an ordinary function that happens to be invoked through a trampoline.

> **Scheduler reality today (multi-carrier by default).** The executor is a carrier pool with per-carrier Chase–Lev work-stealing deques. The carrier count is read once at first spawn from `CAJETA_CARRIERS`, defaulting to `min(nproc, 4)` (clamped to `[1, 16]`), so **spawned tasks run with real wall-clock parallelism by default** — the multi-carrier deadlocks of earlier releases no longer reproduce across the async/threading suite at the default count (`cajeta_runtime.c` carrier-pool block + the R8.3 executor notes). The one limit: a fiber is **pinned to the carrier that first ran it** (cross-carrier resume of a suspended fiber is unsolved), so parallelism comes from fanning out across *distinct* spawned fibers, not from migrating a single one. Set `CAJETA_CARRIERS=1` for deterministic single-carrier execution (debugging / reproducible ordering). A few sync-primitive fast paths and some stale runtime comments still describe the old "single carrier" discipline. The running status lives in `docs/specification/concurrent/AsyncStatus.md`.

This is Java 21's virtual-thread model rather than the stackless state-machine model used by Rust/Swift/C#. We took stackful for two reasons: it ships much faster (no async-fn rewrite pass) and it removes function coloring — any function can call `await`, not just one declared `async`. The per-fiber stack cost is real but matches Java 21's tradeoff; if measurements ever demand it, stackless rewrite remains possible later without changing the surface syntax.

### 2. `scope { ... }` — structured concurrency blocks

`scope` is a block that owns the lifetime of every task spawned inside it. Control doesn't leave the block until every child task has finished or been cancelled.

```
async void processAll(List<Url> urls) {
    scope {
        for (Url u : urls) {
            spawn fetchScore(u);   // launches a child Task<int32>
        }
        // (control reaches here only after every spawn'd task completes)
    }
}
```

Properties:

- **Joins on exit.** ✅ implemented (R5-A): scope_enter at the opening brace pushes a per-fiber (or per-main-thread) scope frame; every spawn site inside the block registers its task's `done` addr; scope_exit at the closing brace iterates the registered list and calls `__cajeta_task_wait` on each before letting control past `}`. Wait is fiber-aware (parks if in a fiber, OS-blocks if main).
- **No leaked tasks.** ✅ implemented (R5-A'): every function body is also an implicit scope — codegen captures the entry scope_top into a watermark alloca, pushes a frame, and at every return path (synthetic fall-through, explicit `return`) calls `__cajeta_scope_exit_to(watermark)` to wait + pop every frame the function pushed. The doc's "spawn without scope = compile error" is satisfied structurally rather than syntactically: every spawn is in *some* scope (the enclosing function's, at minimum). R5-B (rejecting bare top-level spawns at parse time) is therefore subsumed.
- **Cancellation propagates down** — ✅ R5-C implemented. Each fiber carries a `cancel_with` pointer that scope sets to the trigger Throwable when one sibling throws. __cajeta_task_wait checks the marker after each park-resume; if set, the cancelled fiber's await raises the trigger instead of returning. Cancellation is cooperative — only fibers that hit an await observe it; fibers in CPU-bound computation aren't preempted.
- **Errors propagate up** — ✅ R5-D-lite implemented: a child throw records the exception on its Task; scope_exit walks each child's exception slot after wait and re-raises the first one found. Sibling cancellation on first throw still pending R5-C; today's scope waits for every child to complete naturally before raising.
- **Borrows can outlive the spawn** — relies on the join property above. Today the join exists; the static check that rejects borrow captures whose source dies before the scope is a separate piece of work.

`spawn` returns a `Task<T>` that you can optionally `await`. Without `await`, the result is dropped at scope-end (still after the task completes).

To deliberately run a task that outlives the current scope, use `detach`:

```
detach backgroundWork();   // explicit opt-out of scope; rare
```

`detach` requires the spawned function to capture only by `#` transfer (no borrows) — there's no scope to anchor lifetimes to.

### Semantics (v1)

`detach expr;` requires `expr` to be a method-call expression (same shape constraint as `spawn`). The call's value type is irrelevant — `detach` evaluates as `void`; any return value the call would produce is discarded by the runtime.

**Runtime:** the call is enqueued as a fiber via the same `__cajeta_task_run` machinery `spawn` uses. The difference from `spawn`:
- The Task is NOT registered with the enclosing scope (`__cajeta_scope_register` is skipped) — `scope_exit` won't wait for it.
- The Task is NOT pushed onto the drop chain — no scope owns it, so nothing reclaims it. The Task's heap allocation leaks for the process lifetime, matching the explicit "use sparingly" framing in *Open* items below. Captured `#`-transferred values *do* end up owned by the detached task and are freed when the task's locals drop, but the Task wrapper itself leaks.
- The expression's result is `void` — no Task handle escapes back to user code.

**Captures rule.** Every argument to the immediate call must be one of:
- A `#`-transferred value (`MoveExpression`) — explicit ownership transfer.
- A primitive-typed value (int/float/bool family) — value semantics, no aliasing concern.
- A fresh allocator that's auto-promoted in transfer position (a bare `heap T(...)` whose result has no prior identity, per *MemoryModel.md* § Borrow / transfer rules).

A class-typed identifier without `#`, or any expression whose resolved type is a heap class without an explicit transfer marker, is rejected at codegen with `CAJETA_ERROR_DETACH_BORROW_CAPTURE`. The check fires before the trampoline is synthesized; a violation never reaches IR generation.

**Exceptions.** A throw inside a detached task body is captured to the Task's exception slot by the trampoline's existing try/catch (same code path as `spawn`), but nothing awaits the Task, so the exception is silently lost. This matches the doctrine that detach is for fire-and-forget where the caller has explicitly opted out of error propagation. If a detached body needs to surface failures, the body itself must arrange a channel/callback/log of its own choosing.

---

## Sendability — derived from the existing ownership model

A value crosses a thread boundary in exactly two ways:
- **Transferred** via `#` (the existing memory-model operator). The originating thread can't touch the value after; the receiving thread is the new owner. Always safe.
- **Borrowed** into a scoped task (a `spawn` inside a `scope`). The borrow's lifetime is the scope's lifetime; the scope blocks until the task finishes, so the borrow is valid.

The compiler enforces:
- A `spawn` body whose captures include a borrow of `x` requires `x` to live at least as long as the enclosing scope. (The same lifetime analysis used for `MemoryModel.md`'s borrow rules.)
- A `detach` body cannot capture borrows — only `#`-transferred values.

No `Send` / `Sync` traits, no `Sendable` protocol — sendability is what the borrow checker already says about a value's lifetime relative to the consuming thread's lifetime.

---

## Cancellation

Cancellation is cooperative:

- Each fiber carries a `cancel_with` marker (a `void*` Throwable), NULL when not cancelled.
- When one child in a scope throws, `scope_exit` records that throw as the **trigger** and calls `__cajeta_fiber_cancel(sibling, trigger)` on every still-running sibling, storing the trigger in their `cancel_with`.
- `__cajeta_task_wait` checks `cancel_with` on each park-resume (and on entry, to catch an already-done await). If set, it **re-raises the trigger Throwable itself** — there is **no `CancellationException` type**.
- Cleanup happens via the standard `try` / `finally` machinery; the drop chain fires owners on the unwind.

User code never threads a token explicitly. For cancel-on-deadline, `Tasks.withTimeout<R>(Duration, Task<R>)` and `Tasks.withDeadline<R>(int64 deadlineNanos, Task<R>)` run a task under a timer that cooperatively cancels the body on expiry and report present-or-timeout as an `Optional<R>`.

---

## Synchronization primitives

All core sync primitives live in package **`cajeta.concurrent`** — `cajeta.concurrent.Mutex<T>`, `cajeta.concurrent.Lock`, `cajeta.concurrent.RwLock<T>`, `cajeta.concurrent.Semaphore`. The async runtime (`Task<T>`, scope/spawn/detach machinery) and the stdlib types built on the primitives (`Channel<T>`, atomics, `Tasks`) are in the same package. The code examples below omit the package qualifier for brevity.

The data-owning primitives (`Mutex<T>`, `RwLock<T>`, `Semaphore`) use the **closure / scoped form**, not an lvalue guard: you hand a function to `withLock` / `withWrite` / `withPermit`, the primitive runs it as the critical section, and the lock releases when the closure returns *or unwinds through an exception* (a method-scoped `LockGuard` whose drop fires on both edges). The protected value never escapes the closure, so "read/write the data without the lock" is structurally unrepresentable, and "forgot to unlock" can't happen. The lower-level `Lock` (no associated data) is the one primitive that hands out a `LockGuard` lvalue.

These methods are **ordinary (non-`async`) methods** — you call them directly, you do *not* `await` them. When a fiber calls one and the lock is held, the runtime parks the fiber and yields to the carrier (no OS-thread block); main-thread callers `cond_wait`. The fiber-park behaviour is built into the C runtime intrinsics (`Cajeta.lockAcquire` / `lockRelease` / `condvarWait` / `rwlock*`), not the surface API.

Implementation today (R4/R7): each lock holds a pthread mutex (protecting its `held` flag and wait queue) plus a per-lock pthread condvar (for a main-thread acquirer). A fiber that finds the lock held links itself into the wait queue and swaps back to the carrier; release dequeues one waiter onto the ready queue and signals the condvar to wake any main-thread waiter.

### `Mutex<T>` — the fused-lock-and-data primitive

`Mutex<T>` owns the protected value `T`. The only way to touch it is to hand a closure to `withLock`, which receives the current value and stores back what it returns:

```
Mutex<int32> counter = heap Mutex(0);

counter.withLock((n) -> n + 1);   // critical section: read, +1, store back
counter.withLock((n) -> n + 1);
int32 now = counter.get();        // snapshot under the lock -> 2
```

API:

- `Mutex(#T initial)` — construct, taking ownership of `initial`.
- `withLock((T) -> #T fn) -> void` — acquire, pass the current value to `fn`, store `fn`'s result, wake `withLockWhen` waiters, release. Exception-safe.
- `withLockWhen((T) -> boolean cond, (T) -> #T fn) -> void` — wait-for-condition: acquire, then block (each wait atomically releases + parks + reacquires) until `cond(value)` holds, then run `fn` and store. `cond` must be side-effect-free (re-evaluated on every wake). This is the primitive's built-in condition-variable; there is no separate `ConditionVariable` class.
- `get() -> T` — snapshot the value under the lock. Intended for value/primitive `T`; for a heap-class `T` this hands out a reference that outlives the lock, so use `withLock` to operate on mutable `T`.

> A non-blocking `tryLock` and a compute-only `(T) -> R` read variant are noted in the source as later sub-phases — not shipped yet.

**When to use `Mutex<T>`:** shared mutable state with one logical piece of data. The fused pairing means a teammate adding another method that touches the value can't forget to lock — there's no value handle outside `withLock`.

### `Lock` — the no-data variant

When the section to gate is *not* tied to a single piece of data — sequencing two independent things, gating a side-effect-only block — `Lock` provides the same exception-safe release via a `LockGuard` lvalue (the one guard-style primitive):

```
Lock gate = heap Lock();

public void doWork() {
    #LockGuard g = gate.acquire();   // holds the lock; drop releases
    sharedThing.step1();
    log.write("step1 done");
    sharedThing.step2();
    // g drops at scope exit / on throw → release
}
```

API:

- `Lock()` — construct.
- `acquire() -> #LockGuard` — acquire; the returned guard releases on drop.
- `tryAcquire() -> int32` — non-blocking; non-zero on success.
- `releaseLock() -> void` — explicit release (normally the guard drop handles this).

**When to use `Lock`:** the critical section spans more than one logical value, or the gated thing is purely side-effects (e.g. an I/O sequence). Prefer `Mutex<T>` whenever there *is* a single `T` worth pairing.

### `RwLock<T>` — read-heavy state

Many readers can hold the read lock concurrently; a writer waits for outstanding reads to drain, then proceeds exclusively:

```
RwLock<Config> cfg = heap RwLock(loadInitial());

Response handle(Request r) {
    Config snapshot = cfg.read();       // shared read, returns a snapshot
    return process(r, snapshot);
}

void hotReload() {
    Config c = loadFresh();
    cfg.withWrite((old) -> c);          // exclusive write closure
}
```

API:

- `RwLock(#T initial)` — construct.
- `read() -> T` — acquire the read lock, snapshot the value, release, return the snapshot. (Same value/primitive-`T` caveat as `Mutex.get`.)
- `withWrite((T) -> #T fn) -> void` — acquire the write lock exclusively, run `fn` on the current value, store its result, release.

> A non-blocking `tryRead`/`tryWrite` is not exposed today.

**When to use `RwLock<T>`:** read-very-heavy paths where serializing through a `Mutex<T>` would be a measurable bottleneck. Configs, caches with rare invalidation, read-mostly indexes. Reach for it only after measurement — for moderate contention `Mutex<T>` is simpler.

### Wait-for-condition

There is no standalone `ConditionVariable` class. The classic consumer-waits-for-producer pattern is expressed with `Mutex<T>.withLockWhen` — the predicate parks the fiber and reacquires on wake, and any `withLock` store wakes the waiters:

```
Mutex<int32> available = heap Mutex(0);

// Producer: publish one item.
void produce() {
    available.withLock((n) -> n + 1);   // store wakes withLockWhen waiters
}

// Consumer: park until count > 0, then consume one, atomically.
void consume() {
    available.withLockWhen((n) -> n > 0, (n) -> n - 1);
}
```

The predicate is the spurious-wakeup defense — it is re-checked on every wake and only proceeds once it holds.

### Decision tree

When you need to coordinate access to shared state:

1. **Is the state a single piece of data with one logical owner?** → `Mutex<T>` (`withLock`).
2. **Is the section to gate purely about ordering side-effects (no single value to pair the lock with)?** → `Lock`.
3. **Reads vastly dominate writes, and measurement shows `Mutex<T>` is the bottleneck?** → `RwLock<T>`.
4. **Need wait-for-condition (consumer-waits-for-producer style)?** → `Mutex<T>.withLockWhen`.
5. **Counted permit pool with overlapping lifetimes that no scope can bound?** → `Semaphore` (`withPermit`).

Reach for the highest-numbered tool only when the lower-numbered ones don't fit. Each step down is a step toward subtler bugs and higher cognitive load.

---

## Standard library types (not language primitives)

The threading core stops at the sync primitives above (`Mutex<T>`, `Lock`, `RwLock<T>`, and `Mutex.withLockWhen` for condition-waiting). The standard library provides higher-level building blocks composed from them — `Semaphore`, `Channel<T>`, the atomics, and `Tasks` — none of which need special compiler knowledge.

### `Channel<T>` — bounded MPMC queue

```
Channel<int32> ch = heap Channel<int32>(8);   // bounded ring buffer, capacity 8

scope {
    spawn () -> async void {
        for (int32 i = 0; i < 10; i = i + 1) {
            ch.send(i);          // parks the fiber while full
        }
        ch.close();
    };
    spawn () -> async void {
        Optional<int32> v = ch.receive();
        while (v.isPresent()) {
            // ... use v.get() ...
            v = ch.receive();    // empty Optional once closed AND drained
        }
    };
}
```

A `Channel<T>` is a bounded ring buffer (capacity fixed at construction) guarded internally by the lock + condvar intrinsics. `send` and `receive` are plain (non-`async`) methods that park the calling fiber when the buffer is full / empty. API: `send(T)`, `receive() -> Optional<T>`, `tryReceive() -> Optional<T>` (non-blocking), `close()`, `isClosed() -> boolean`. `close` is one-way; `receive` drains buffered items, then returns an empty `Optional` once closed-and-drained. v1 targets value/primitive `T` — heap-class items still buffered at destruction time aren't dropped, so drain via a `receive()` loop before dropping the channel.

### `Semaphore` — counting permit pool

```
Semaphore s = heap Semaphore(5);

Result useResource() {
    return s.withPermit(() -> expensiveCall());   // acquire, run, release
}
```

API: `Semaphore(int32 initial)`, `acquire() -> void`, `release() -> void`, `withPermit(() -> void fn) -> void` (the scoped form — acquire, run `fn`, release), `availablePermits() -> int32`. Use case: bound the *number* of concurrent operations against an unbounded set of consumers (e.g. a server limiting global concurrency across all requests).

**Often you don't need a semaphore.** If the workload is statically batchable into "K at a time," chunked `scope { spawn N times }` expresses the limit in the program's shape with no permit bookkeeping. Reach for `Semaphore` when the consumers are unbounded and no scope can serve as the batching boundary.

### `AtomicInt32` / `AtomicInt64` — single-word atomics

```
AtomicInt32 hits = heap AtomicInt32(0);

// any fiber
hits.fetchAdd(1);

// any fiber
int32 snapshot = hits.load();
```

Owns a heap-allocated word that compiler-emitted inline LLVM atomic
instructions operate against (`load atomic`, `store atomic`, `atomicrmw
add`, `cmpxchg`). The default no-suffix methods are sequentially
consistent; named ordered variants opt into a weaker ordering.

Default (seq_cst) API:
- `load() → T` — atomic read.
- `store(T v) → void` — atomic write.
- `fetchAdd(T delta) → T` — atomic add, returns the value the cell held
  before the add. Use for sequence numbers and counters.
- `compareAndSet(T expected, T desired) → boolean` — atomic CAS,
  returns whether the swap actually happened. Use as the lock-free
  retry-loop building block.

Fixed-ordering variants (R8.1b — named, not enum-parameterized, because
LLVM atomic IR bakes the ordering at instruction construction; a
runtime-variable `MemoryOrder` would force a per-op `switch` that only
collapses through inlining, which is unpredictable at lower opt
levels):
- `loadRelaxed() / loadAcquire()` — RELAXED for stats / counters where
  ordering doesn't matter; ACQUIRE pairs with `storeRelease` for
  publish/subscribe handshakes on a sentinel flag.
- `storeRelaxed(v) / storeRelease(v)` — RELAXED for owner-only writes
  (e.g. the deque's bottom pointer from its owning carrier); RELEASE
  for publishes the consumer pairs with an ACQUIRE load.
- `fetchAddRelaxed(delta)` — relaxed atomic add, the right call for
  stat counters under contention (atomicity without ordering).
- `compareAndSetAcquire(exp, des)` — CAS with Acquire on both success
  and failure. The stealer-side race in Chase-Lev needs exactly this
  shape.

The surface only enumerates the orderings the work-stealing deque
(R8.2) is about to need; add more named variants when a concrete
consumer surfaces.

Backs the lock-free runtime data structures (Chase-Lev deque,
Treiber-style free lists) that the work-stealing pool — R8 § Executor —
needs to scale beyond a single carrier.

---

## Deferred: `actor` (v2+)

An earlier draft of this spec had `actor` as a third core primitive — a type kind with serialized state and an implicit mailbox. The decision for v1 is to **defer it** and revisit once `Mutex<T>` + the rest of the threading runtime have soaked. The actor pattern can already be expressed manually:

```
class Counter {
    private Mutex<int32> value = heap Mutex(0);

    public int32 next() {
        value.withLock((n) -> n + 1);
        return value.get();
    }
}
```

The value of a language-level `actor` keyword over this manual form is compiler-enforced encapsulation: an `actor` would guarantee the private state has no escape hatch, and would automatically route every public method through the mailbox so callers can't bypass it. With the manual form, encapsulation rests on `private` + reviewed code — a teammate adding a method that touches `value` outside a `lock()` block silently breaks the invariant.

The decision to defer is pragmatic: we want to see how often this pattern appears in real Cajeta code, and whether the manual form is enough for most cases. If actors become a common pattern, the compiler can later add `actor` as sugar over the same lowering — at that point it's additive, not a breaking change.

Channels, similarly, used to be presented as complementary to actors. They're now positioned simply as a stdlib type useful in producer/consumer patterns; nothing about them requires special compiler support.

---

## Examples

### Single-threaded `async` (event-loop style)

A program with no `spawn` is just an event loop; `await` is a coroutine yield.

```
async int32 main() {
    Response a = await http.get(urlA);
    Response b = await http.get(urlB);
    return a.bytes + b.bytes;
}
```

### Parallel fan-out with structured joins

```
async int32 sumScores(List<Url> urls) {
    Mutex<int32> total = heap Mutex(0);
    scope {
        for (Url u : urls) {
            spawn () -> async void {
                int32 s = await fetchScore(u);
                total.withLock((n) -> n + s);
            };
        }
    }
    return total.get();
}
```

### Bounded producer / consumer

```
async void pipeline(Source src, Sink sink) {
    Channel<Record> ch = heap Channel<Record>(64);
    scope {
        spawn () -> async void {
            Optional<Record> r = src.next();
            while (r.isPresent()) {
                ch.send(r.get());
                r = src.next();
            }
            ch.close();
        };
        spawn () -> async void {
            Optional<Record> r = ch.receive();
            while (r.isPresent()) {
                await sink.write(r.get());
                r = ch.receive();
            }
        };
    }
}
```

### Timeout

`Tasks.withTimeout` runs a `Task<R>` under a deadline and reports present-or-timeout as an `Optional<R>`:

```
async Optional<Response> getWithTimeout(Url u, Duration d) {
    return Tasks.withTimeout(d, spawn http.get(u));
}
```

---

## Runtime requirements

A small C runtime ships in `runtime/native/`, paralleling the existing exception / drop-chain helpers:

- **Mutex / Lock / RwLock primitives** — wrappers over the platform's pthread primitives (`pthread_mutex_t`, `pthread_rwlock_t`). The async-aware suspending behaviour layers a wait queue on top — the OS lock is held only across the actual critical section, not during the user-task's suspension.
- **Condition variable primitive** — wrapper over `pthread_cond_t` with the same async-aware wait queue.
- **Fiber primitive** — `__cajeta_task_run(ctx, trampoline)` allocates a stackful fiber (its own ~64 KB stack + `ucontext_t`) and queues it on the ready list. `__cajeta_task_wait(&task->done)` parks the current fiber if called from inside one (cooperative yield to the carrier), or `pthread_cond_wait`s on the global done-condvar if called from the main thread. `__cajeta_task_complete(&task->done)` flips the flag, broadcasts the done-condvar, and moves every parked fiber back to the ready queue so they can recheck their await conditions.
- **Executor** — N OS-thread carriers, each owning a Chase-Lev work-stealing deque (`cajeta_carrier_deque`): atomic `top` / `bottom` sequence numbers over a fixed-size circular slot array, single-producer / multi-consumer. Owner-side (`push_bottom` / `pop_bottom`) drains LIFO — the cache-warm end. The `steal` operation (FIFO from the deque's perspective) is what peer carriers call when their own deque empties. R8.3 — N defaults to `min(_SC_NPROCESSORS_ONLN, 4)` and is overridden by `$CAJETA_CARRIERS` at the first `__cajeta_task_run`, clamped to `[1, CAJETA_MAX_CARRIERS=16]`. Owner-side pushes still take a per-carrier `deque_mutex` for the cross-thread case (main thread / lock-release / condvar-notify pushing into a foreign deque), preserving Chase-Lev's single-producer contract under the mutex. The pool condvar (`__cajeta_task_queue_cond`) wakes idle carriers when work appears anywhere in the pool. The earlier multi-carrier deadlock is no longer reproducible across the full async/threading battery under N=4.
- **Cancellation marker** — each fiber carries a `cancel_with` `void*` (the trigger Throwable), set by `__cajeta_fiber_cancel`; `__cajeta_task_wait` checks it on resume and re-raises (R5-C, shipped). No separate atomic token type.

The user-facing `cajeta.concurrent.*` classes (`Mutex`, `Lock`, `RwLock`, `Semaphore`, `Channel`, `Tasks`, the atomics) wrap these C helpers — the runtime entry points (`Cajeta.lockNew()` / `Cajeta.lockAcquire(p)` / `Cajeta.condvarWait(...)` / `Cajeta.rwlock*` / etc.) are an implementation detail. User code should target the class API, not the intrinsics.

Suspension is delivered by `ucontext.h` (`getcontext` / `makecontext` / `swapcontext`). The compiler doesn't transform `async fn` bodies at all — they compile like ordinary functions; the trampoline emitted at each spawn site wraps the call and runs inside the spawned fiber's stack.

---

## Interaction with the rest of the language

| Feature | Threading interaction |
|--|--|
| `#` transfer | The ownership-move marker doubles as the "send across threads" marker. Cross-thread transfer is what `#` already means at scope boundaries. |
| Templates | `Mutex<T>`, `RwLock<T>`, `Task<T>` monomorphize identically to class templates — no erasure, primitives allowed. |
| Drop chain | `withLock`/`withWrite`/`withPermit` release on closure return or exception unwind (via an internal method-scoped `LockGuard`); `Lock.acquire` hands out a `LockGuard` whose drop releases. Task frame drop fires the scope-end drops in the right order. |
| Exceptions | Async exceptions raise on `await`; the setjmp/longjmp-based catch chain unwinds through the suspension stack. The lock primitives' internal guards drop on the unwind path, so unlocking-on-exception is automatic. |
| Method references | `obj::lockedMethod` works for methods that themselves use `Mutex<T>` — nothing thread-specific in the method reference itself. |

---

## Known gaps / open questions

- **Actor sugar.** Deferred to v2+; see the dedicated section. The interesting design question is whether the keyword would generate a synthesized `Mutex<state>` wrapper class or take a different lowering (e.g. one OS thread per actor, mailbox queue, dispatcher).
- **Async iteration.** `cajeta.concurrent.AsyncIterator<T>` interface has shipped (R9.7) with the canonical user-side loop pattern documented (`while ((opt = iter.next()).isPresent()) ...`); a `for (T x in iter) { ... }` desugaring over the same interface is the remaining v2.1 surface, deferred. Multi-call iteration through an interface-typed receiver hits the M5(b) function-pointer/sret ripple gap and is tracked separately — concrete-typed receivers loop cleanly today.
- **Pinning.** Some runtime objects (e.g. interop with OS callbacks) may need to be pinned to a thread. v1 has no pinning; revisit if FFI threading becomes a concern.
- **`runBlocking` escape hatch.** ✅ Shipped as `cajeta.concurrent.Tasks.runBlocking<R>(() -> R body) -> R`. The runtime's `__cajeta_task_wait` falls through to a condvar wait when the caller has no current fiber, so a plain non-async `main` can drive async work via `Tasks.runBlocking(() -> { ...await... })` without itself being `async`. v1 surface covers primitive / value-return R via the spawn-of-lambda ABI; heap-return callers keep using the explicit `await spawn body()` form.
- **Channel select.** ✅ Shipped as `cajeta.concurrent.Tasks.selectReceive<T>(Channel<T>[]) -> Optional<SelectResult<T>>` (R9.6). Returns a present `SelectResult` with the index and value of the channel that fired; empty once every channel is closed and drained. The `select { case <- a; case <- b }` keyword form remains deferred — the stdlib API covers the same multiplexed-receive use case.
- **Detached task ownership and leaks.** `detach` consumes captures by `#`. The detached task's lifetime is the runtime's lifetime; nothing reclaims its result. Use sparingly.

---

## Implementation status

Full runtime status lives in `docs/specification/concurrent/AsyncStatus.md`. As of
recent commits, R1 through R9 plus error-model v1 have shipped:
stackful fibers on a carrier pool with per-carrier Chase–Lev
work-stealing deques (`CAJETA_CARRIERS=N`, default `min(nproc, 4)`) —
**multi-carrier by default, so spawned tasks run in parallel** (fibers are
pinned to their home carrier; `CAJETA_CARRIERS=1` forces single-carrier; see
the scheduler note under `async fn`). Also: explicit `scope { }` + implicit function-body scopes
with join-on-exit, R5-C cooperative cancellation, R5-D scope
exception-escalation, atomic ints (R8.1), Chase–Lev deques (R8.2), the
carrier pool plumbing (R8.3), a fiber-aware timer (R9.1 — backing
`Duration` / `Tasks.withTimeout` / `Tasks.withDeadline`), and an
epoll-based async I/O reactor (R9.4).

The user-facing surface also gained spawn-of-lambda — `SpawnExpression`
accepts a function-typed scope field as the inner call, letting
stdlib utility methods (like `Tasks.withTimeout`) spawn a callable
the user passed in. v1 restricts to heap-ownership / primitive return.

## Pinning tests

| Surface | Tests |
| ------- | ----- |
| `Task<T>` typing | `test/parser/TaskTypingTests.cpp` |
| `spawn` syntax + drop semantics | `test/parser/SpawnDropTests.cpp`, `test/parser/AsyncSyntaxTests.cpp` |
| `spawn` of a function-typed local | `test/parser/SpawnLambdaTests.cpp` |
| `detach` syntax | `test/parser/DetachTests.cpp` |
| Per-fiber drop chain | `test/parser/PerFiberDropChainTests.cpp` |
| `Lock` class + intrinsics | `test/parser/LockIntrinsicTests.cpp`, `test/parser/LockClassTests.cpp` |
| `Mutex` / `RwLock` / `Semaphore` | `test/parser/MutexTests.cpp`, `test/parser/RwLockTests.cpp`, `test/parser/SemaphoreTests.cpp` |
| `Channel<T>` | `test/parser/ChannelTests.cpp` |
| `Channel.select` (Tasks.selectReceive) | `test/parser/ChannelSelectTests.cpp` |
| `AsyncIterator<T>` | `test/parser/AsyncIteratorTests.cpp` |
| Atomics | `test/parser/AtomicTests.cpp` |
| Timer + `Duration` | `test/parser/TimerTests.cpp`, `test/parser/DurationTests.cpp` |
| `withTimeout` / `withDeadline` | `test/parser/WithTimeoutTests.cpp`, `test/parser/WithDeadlineTests.cpp` |
| `runBlocking` | `test/parser/RunBlockingTests.cpp` |
| I/O reactor | `test/parser/IoReactorTests.cpp` |

## Open items

Surface classes `Fiber` and `Thread` haven't been declared in
cajeta-source form yet (the runtime is shipped; the wrappers are
designed). Tracked in `Features.md` as S-804.

V2 candidates listed as known gaps but not yet scheduled: a
`for (T x in iter)` syntactic desugaring over `AsyncIterator<T>` and
`actor`-style sugar over the sync primitives. The `runBlocking` escape
hatch, `Channel.select` multiplex, and the `AsyncIterator<T>` contract
itself have shipped (R9.5 / R9.6 / R9.7; see `Tasks.runBlocking`,
`Tasks.selectReceive`, and `AsyncIterator`).
