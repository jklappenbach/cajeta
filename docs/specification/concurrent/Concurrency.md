# Cajeta Concurrency Model — Specification v1

## Goals

- **No data races at compile time.** The ownership/borrow machinery already used for single-threaded memory safety extends to thread boundaries, so sharing mutable state without serialization is rejected before codegen.
- **Structured concurrency by default.** Every spawned task has a scope. Scopes do not return until their children complete or are cancelled. "Fire and forget" requires explicit opt-in.
- **No SAM interfaces for tasks.** Threading primitives consume function-typed values. There is no `Runnable`, no `Callable`, and no `Future.get()` vs `CompletableFuture.then*` schism.
- **No `synchronized`, no `volatile`, no `ThreadLocal`.** Each of those Java warts comes from sharing mutable state across threads, and Cajeta makes that the unusual case. Locks exist only through the RAII-guard primitives below, so there is no separate "acquire / forget to release" API. The legitimate use of `ThreadLocal` is ambient per-request state, and the sound, fiber-keyed replacement for it is `FiberLocal`; see [`FiberLocal.md`](FiberLocal.md).
- **No GC, no runtime bootstrap.** Task state machines lower to plain structs allocated on the single-owner heap. The runtime is a small set of C functions for the executor, the scheduler, and the sync primitives, in the same style as the existing `__cajeta_*` helpers.

## Non-goals (v1)

- Preemptive cancellation. Cancellation is cooperative, so a task observes a cancel signal at its next `await`.
- Custom executors beyond the built-in scheduler. The executor is a carrier pool with per-carrier work-stealing deques, sized via `CAJETA_CARRIERS` and multi-carrier by default at `min(nproc, 4)`; see the scheduler note under `async fn`. Replacing it wholesale is not in scope.
- Distributed actors (RPC across machines).
- Effect handlers / algebraic effects.
- Async iteration / streaming (`AsyncIterator` style). It is covered separately if added.

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

`await` is only legal inside `async fn` / inside a `spawn` body. Calling an `async fn` without `await` produces an unstarted `Task<T>`, which does nothing until awaited or `spawn`ed.

Under the hood, each task is a stackful fiber with its own heap-allocated stack (~64 KB initial) plus a saved `ucontext`. `await` parks the running fiber and switches back to its carrier, which picks another ready fiber off its deque. No `async fn` codegen transformation is required. Suspension lives in the C runtime's context-switch primitive, and from the compiler's perspective an `async fn` is an ordinary function that happens to be invoked through a trampoline.

> **Scheduler reality today.** The executor is a carrier pool with per-carrier Chase–Lev work-stealing deques, and it is multi-carrier by default. The carrier count is read once at first spawn from `CAJETA_CARRIERS`, defaulting to `min(nproc, 4)` and clamped to `[1, 16]`, so spawned tasks run with real wall-clock parallelism by default. The multi-carrier deadlocks of earlier releases no longer reproduce across the async/threading suite at the default count (`cajeta_runtime.c` carrier-pool block + the R8.3 executor notes). The one limit is that a fiber is pinned to the carrier that first ran it, because cross-carrier resume of a suspended fiber is unsolved. Parallelism therefore comes from fanning out across distinct spawned fibers, since a single fiber never migrates. Set `CAJETA_CARRIERS=1` for deterministic single-carrier execution when debugging or when you need reproducible ordering. A few sync-primitive fast paths and some stale runtime comments still describe the old "single carrier" discipline. The running status lives in `docs/specification/concurrent/AsyncStatus.md`.

This is Java 21's virtual-thread model. Rust, Swift, and C# use the stackless state-machine model instead. We took stackful for two reasons. It ships much faster, since there is no async-fn rewrite pass, and it removes function coloring, so any function can call `await` whether or not it is declared `async`. The per-fiber stack cost is real but matches Java 21's tradeoff. If measurements ever demand it, a stackless rewrite remains possible later without changing the surface syntax.

### 2. `scope { ... }` — structured concurrency blocks

`scope` is a block that owns the lifetime of every task spawned inside it. Control does not leave the block until every child task has finished or been cancelled.

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

- **Joins on exit.** ✅ implemented (R5-A). scope_enter at the opening brace pushes a per-fiber or per-main-thread scope frame. Every spawn site inside the block registers its task's `done` addr. scope_exit at the closing brace iterates the registered list and calls `__cajeta_task_wait` on each before letting control past `}`. The wait is fiber-aware, so it parks in a fiber and OS-blocks on main.
- **No leaked tasks.** ✅ implemented (R5-A'). Every function body is also an implicit scope. Codegen captures the entry scope_top into a watermark alloca, pushes a frame, and at every return path, whether a synthetic fall-through or an explicit `return`, calls `__cajeta_scope_exit_to(watermark)` to wait on and pop every frame the function pushed. The doc's "spawn without scope = compile error" is satisfied structurally rather than syntactically, because every spawn is in some scope, the enclosing function's at minimum. R5-B, which would reject bare top-level spawns at parse time, is therefore subsumed.
- **Cancellation propagates down.** ✅ R5-C implemented. Each fiber carries a `cancel_with` pointer that scope sets to the trigger Throwable when one sibling throws. __cajeta_task_wait checks the marker after each park-resume, and if it is set, the cancelled fiber's await raises the trigger instead of returning. Cancellation is cooperative. Only fibers that hit an await observe it, and fibers in CPU-bound computation are not preempted.
- **Errors propagate up.** ✅ R5-D-lite implemented. A child throw records the exception on its Task, and scope_exit walks each child's exception slot after the wait and re-raises the first one found. Sibling cancellation on the first throw is still pending R5-C, so today's scope waits for every child to complete naturally before raising.
- **Borrows can outlive the spawn.** This relies on the join property above. Today the join exists, and the static check that rejects borrow captures whose source dies before the scope is a separate piece of work.

`spawn` returns a `Task<T>` that you can optionally `await`. Without `await`, the result is dropped at scope-end, which is still after the task completes.

To deliberately run a task that outlives the current scope, use `detach`:

```
detach backgroundWork();   // explicit opt-out of scope; rare
```

`detach` requires the spawned function to capture only by `#` transfer, with no borrows, because there is no scope to anchor lifetimes to.

### Semantics (v1)

`detach expr;` requires `expr` to be a method-call expression, the same shape constraint as `spawn`. The call's value type is irrelevant. `detach` evaluates as `void`, and any return value the call would produce is discarded by the runtime.

**Runtime.** The call is enqueued as a fiber via the same `__cajeta_task_run` machinery `spawn` uses. It differs from `spawn` in three ways:
- The Task is not registered with the enclosing scope, since `__cajeta_scope_register` is skipped, so `scope_exit` will not wait for it.
- The Task is not pushed onto the drop chain. No scope owns it, so nothing reclaims it. The Task's heap allocation leaks for the process lifetime, matching the explicit "use sparingly" framing in *Open* items below. Captured `#`-transferred values do end up owned by the detached task and are freed when the task's locals drop, but the Task wrapper itself leaks.
- The expression's result is `void`, so no Task handle escapes back to user code.

**Captures rule.** Every argument to the immediate call must be one of:
- A `#`-transferred value (`MoveExpression`), which is an explicit ownership transfer.
- A primitive-typed value (int/float/bool family), which has value semantics and no aliasing concern.
- A fresh allocator that is auto-promoted in transfer position, meaning a bare `heap T(...)` whose result has no prior identity, per *MemoryModel.md* § Borrow / transfer rules.

A class-typed identifier without `#`, or any expression whose resolved type is a heap class without an explicit transfer marker, is rejected at codegen with `CAJETA_ERROR_DETACH_BORROW_CAPTURE`. The check fires before the trampoline is synthesized, so a violation never reaches IR generation.

**Exceptions.** A throw inside a detached task body is captured to the Task's exception slot by the trampoline's existing try/catch, the same code path as `spawn`, but nothing awaits the Task, so the exception is silently lost. This matches the doctrine that detach is for fire-and-forget where the caller has explicitly opted out of error propagation. If a detached body needs to surface failures, the body itself must arrange a channel, callback, or log of its own choosing.

---

## Sendability — derived from the existing ownership model

A value crosses a thread boundary in exactly two ways:
- **Transferred** via `#`, the existing memory-model operator. The originating thread cannot touch the value afterward, and the receiving thread is the new owner. This is always safe.
- **Borrowed** into a scoped task, meaning a `spawn` inside a `scope`. The borrow's lifetime is the scope's lifetime, and the scope blocks until the task finishes, so the borrow is valid.

The compiler enforces two rules:
- A `spawn` body whose captures include a borrow of `x` requires `x` to live at least as long as the enclosing scope. This is the same lifetime analysis used for `MemoryModel.md`'s borrow rules.
- A `detach` body cannot capture borrows. It can capture only `#`-transferred values.

There are no `Send` / `Sync` traits and no `Sendable` protocol. Sendability is what the borrow checker already says about a value's lifetime relative to the consuming thread's lifetime.

---

## Cancellation

Cancellation is cooperative:

- Each fiber carries a `cancel_with` marker, a `void*` Throwable, which is NULL when not cancelled.
- When one child in a scope throws, `scope_exit` records that throw as the trigger and calls `__cajeta_fiber_cancel(sibling, trigger)` on every still-running sibling, storing the trigger in their `cancel_with`.
- `__cajeta_task_wait` checks `cancel_with` on each park-resume, and on entry to catch an already-done await. If it is set, it re-raises the trigger Throwable itself. There is no `CancellationException` type.
- Cleanup happens via the standard `try` / `finally` machinery, and the drop chain fires owners on the unwind.

User code never threads a token explicitly. For cancel-on-deadline, `Tasks.withTimeout<R>(Duration, Task<R>)` and `Tasks.withDeadline<R>(int64 deadlineNanos, Task<R>)` run a task under a timer that cooperatively cancels the body on expiry and report present-or-timeout as an `Optional<R>`.

---

## Synchronization primitives

All core sync primitives live in package `cajeta.concurrent`. They are `cajeta.concurrent.Mutex<T>`, `cajeta.concurrent.Lock`, `cajeta.concurrent.RwLock<T>`, and `cajeta.concurrent.Semaphore`. The async runtime (`Task<T>` and the scope/spawn/detach machinery) and the stdlib types built on the primitives (`Channel<T>`, the atomics, `Tasks`) are in the same package. The code examples below omit the package qualifier for brevity.

The data-owning primitives (`Mutex<T>`, `RwLock<T>`, `Semaphore`) use the closure / scoped form. You hand a function to `withLock` / `withWrite` / `withPermit`, the primitive runs it as the critical section, and the lock releases when the closure returns or unwinds through an exception, through a method-scoped `LockGuard` whose drop fires on both edges. The protected value never escapes the closure, so "read/write the data without the lock" is structurally unrepresentable, and "forgot to unlock" cannot happen. The lower-level `Lock`, which has no associated data, is the one primitive that hands out a `LockGuard` lvalue.

These methods are ordinary, non-`async` methods. You call them directly and never `await` them. When a fiber calls one and the lock is held, the runtime parks the fiber and yields to the carrier without blocking an OS thread, and main-thread callers `cond_wait`. The fiber-park behaviour is built into the C runtime intrinsics (`Cajeta.lockAcquire` / `lockRelease` / `condvarWait` / `rwlock*`) below the surface API.

In today's implementation (R4/R7), each lock holds a pthread mutex that protects its `held` flag and wait queue, plus a per-lock pthread condvar for a main-thread acquirer. A fiber that finds the lock held links itself into the wait queue and swaps back to the carrier. Release dequeues one waiter onto the ready queue and signals the condvar to wake any main-thread waiter.

### `Mutex<T>` — the fused-lock-and-data primitive

`Mutex<T>` owns the protected value `T`. The only way to touch it is to hand a closure to `withLock`, which receives the current value and stores back what it returns:

```
Mutex<int32> counter = heap Mutex(0);

counter.withLock((n) -> n + 1);   // critical section: read, +1, store back
counter.withLock((n) -> n + 1);
int32 now = counter.get();        // snapshot under the lock -> 2
```

API:

- `Mutex(#T initial)` constructs the mutex, taking ownership of `initial`.
- `withLock((T) -> #T fn) -> void` acquires, passes the current value to `fn`, stores `fn`'s result, wakes `withLockWhen` waiters, and releases. It is exception-safe.
- `withLockWhen((T) -> boolean cond, (T) -> #T fn) -> void` is wait-for-condition. It acquires, then blocks until `cond(value)` holds, where each wait atomically releases, parks, and reacquires, and then runs `fn` and stores. `cond` must be side-effect-free, since it is re-evaluated on every wake. This is the primitive's built-in condition variable, and there is no separate `ConditionVariable` class.
- `get() -> T` snapshots the value under the lock. It is intended for value/primitive `T`. For a heap-class `T` this hands out a reference that outlives the lock, so use `withLock` to operate on mutable `T`.

> A non-blocking `tryLock` and a compute-only `(T) -> R` read variant are noted in the source as later sub-phases. They have not shipped yet.

**When to use `Mutex<T>`.** Use it for shared mutable state with one logical piece of data. The fused pairing means a teammate adding another method that touches the value cannot forget to lock, because there is no value handle outside `withLock`.

### `Lock` — the no-data variant

Some sections to gate are not tied to a single piece of data, such as sequencing two independent things or gating a side-effect-only block. For those, `Lock` provides the same exception-safe release via a `LockGuard` lvalue, and it is the one guard-style primitive:

```
Lock gate = heap Lock();

public void doWork() {
    LockGuard g #= gate.acquire();   // holds the lock; drop releases
    sharedThing.step1();
    log.write("step1 done");
    sharedThing.step2();
    // g drops at scope exit / on throw → release
}
```

API:

- `Lock()` constructs the lock.
- `acquire() -> #LockGuard` acquires, and the returned guard releases on drop.
- `tryAcquire() -> int32` is non-blocking and returns non-zero on success.
- `releaseLock() -> void` releases explicitly. Normally the guard drop handles this.

**When to use `Lock`.** Use it when the critical section spans more than one logical value, or when the gated thing is purely side-effects, such as an I/O sequence. Prefer `Mutex<T>` whenever there is a single `T` worth pairing.

### `RwLock<T>` — read-heavy state

Many readers can hold the read lock concurrently. A writer waits for outstanding reads to drain, then proceeds exclusively:

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

- `RwLock(#T initial)` constructs the lock.
- `read() -> T` acquires the read lock, snapshots the value, releases, and returns the snapshot. The same value/primitive-`T` caveat as `Mutex.get` applies.
- `withWrite((T) -> #T fn) -> void` acquires the write lock exclusively, runs `fn` on the current value, stores its result, and releases.

> A non-blocking `tryRead`/`tryWrite` is not exposed today.

**When to use `RwLock<T>`.** Use it on read-very-heavy paths where serializing through a `Mutex<T>` would be a measurable bottleneck, such as configs, caches with rare invalidation, and read-mostly indexes. Reach for it only after measurement, since for moderate contention `Mutex<T>` is simpler.

### Wait-for-condition

There is no standalone `ConditionVariable` class. The classic consumer-waits-for-producer pattern is expressed with `Mutex<T>.withLockWhen`. The predicate parks the fiber and reacquires on wake, and any `withLock` store wakes the waiters:

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

The predicate is the spurious-wakeup defense. It is re-checked on every wake and only proceeds once it holds.

### Decision tree

When you need to coordinate access to shared state:

1. **Is the state a single piece of data with one logical owner?** → `Mutex<T>` (`withLock`).
2. **Is the section to gate purely about ordering side-effects (no single value to pair the lock with)?** → `Lock`.
3. **Reads vastly dominate writes, and measurement shows `Mutex<T>` is the bottleneck?** → `RwLock<T>`.
4. **Need wait-for-condition (consumer-waits-for-producer style)?** → `Mutex<T>.withLockWhen`.
5. **Counted permit pool with overlapping lifetimes that no scope can bound?** → `Semaphore` (`withPermit`).

Reach for the highest-numbered tool only when the lower-numbered ones do not fit. Each step down is a step toward subtler bugs and higher cognitive load.

---

## Standard library types (not language primitives)

The threading core stops at the sync primitives above (`Mutex<T>`, `Lock`, `RwLock<T>`, and `Mutex.withLockWhen` for condition-waiting). The standard library provides higher-level building blocks composed from them, namely `Semaphore`, `Channel<T>`, the atomics, and `Tasks`. None of them needs special compiler knowledge.

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

A `Channel<T>` is a bounded ring buffer, with its capacity fixed at construction, guarded internally by the lock and condvar intrinsics. `send` and `receive` are plain, non-`async` methods that park the calling fiber when the buffer is full or empty. The API is `send(T)`, `receive() -> Optional<T>`, the non-blocking `tryReceive() -> Optional<T>`, `close()`, and `isClosed() -> boolean`. `close` is one-way. `receive` drains buffered items, then returns an empty `Optional` once closed and drained. v1 targets value/primitive `T`. Heap-class items still buffered at destruction time are not dropped, so drain via a `receive()` loop before dropping the channel.

### `Semaphore` — counting permit pool

```
Semaphore s = heap Semaphore(5);

Result useResource() {
    return s.withPermit(() -> expensiveCall());   // acquire, run, release
}
```

The API is `Semaphore(int32 initial)`, `acquire() -> void`, `release() -> void`, `withPermit(() -> void fn) -> void`, which is the scoped form that acquires, runs `fn`, and releases, and `availablePermits() -> int32`. The use case is bounding the number of concurrent operations against an unbounded set of consumers, such as a server limiting global concurrency across all requests.

Often you do not need a semaphore. If the workload is statically batchable into "K at a time," chunked `scope { spawn N times }` expresses the limit in the program's shape with no permit bookkeeping. Reach for `Semaphore` when the consumers are unbounded and no scope can serve as the batching boundary.

### `AtomicInt32` / `AtomicInt64` — single-word atomics

```
AtomicInt32 hits = heap AtomicInt32(0);

// any fiber
hits.fetchAdd(1);

// any fiber
int32 snapshot = hits.load();
```

Each owns a heap-allocated word that compiler-emitted inline LLVM atomic
instructions operate against (`load atomic`, `store atomic`, `atomicrmw
add`, `cmpxchg`). The default no-suffix methods are sequentially
consistent, and the named ordered variants opt into a weaker ordering.

Default (seq_cst) API:
- `load() → T` is an atomic read.
- `store(T v) → void` is an atomic write.
- `fetchAdd(T delta) → T` is an atomic add that returns the value the cell
  held before the add. Use it for sequence numbers and counters.
- `compareAndSet(T expected, T desired) → boolean` is an atomic CAS that
  returns whether the swap actually happened. Use it as the lock-free
  retry-loop building block.

The fixed-ordering variants (R8.1b) are named methods, because LLVM
atomic IR bakes the ordering at instruction construction. An
enum-parameterized form with a runtime-variable `MemoryOrder` would force
a per-op `switch` that only collapses through inlining, which is
unpredictable at lower opt levels:
- `loadRelaxed() / loadAcquire()`. RELAXED is for stats and counters where
  ordering does not matter. ACQUIRE pairs with `storeRelease` for
  publish/subscribe handshakes on a sentinel flag.
- `storeRelaxed(v) / storeRelease(v)`. RELAXED is for owner-only writes,
  such as the deque's bottom pointer from its owning carrier. RELEASE is
  for publishes the consumer pairs with an ACQUIRE load.
- `fetchAddRelaxed(delta)` is a relaxed atomic add, the right call for
  stat counters under contention, where atomicity matters and ordering
  does not.
- `compareAndSetAcquire(exp, des)` is a CAS with Acquire on both success
  and failure. The stealer-side race in Chase-Lev needs exactly this
  shape.

The surface only enumerates the orderings the work-stealing deque
(R8.2) is about to need. Add more named variants when a concrete
consumer surfaces.

The atomics back the lock-free runtime data structures (Chase-Lev deque,
Treiber-style free lists) that the work-stealing pool of R8 § Executor
needs to scale beyond a single carrier.

---

## Deferred: `actor` (v2+)

An earlier draft of this spec had `actor` as a third core primitive, a type kind with serialized state and an implicit mailbox. The decision for v1 is to defer it and revisit once `Mutex<T>` and the rest of the threading runtime have soaked. The actor pattern can already be expressed manually:

```
class Counter {
    private Mutex<int32> value = heap Mutex(0);

    public int32 next() {
        value.withLock((n) -> n + 1);
        return value.get();
    }
}
```

The value of a language-level `actor` keyword over this manual form is compiler-enforced encapsulation. An `actor` would guarantee the private state has no escape hatch, and it would automatically route every public method through the mailbox so callers cannot bypass it. With the manual form, encapsulation rests on `private` and reviewed code, so a teammate adding a method that touches `value` outside a `lock()` block silently breaks the invariant.

The decision to defer is pragmatic. We want to see how often this pattern appears in real Cajeta code, and whether the manual form is enough for most cases. If actors become a common pattern, the compiler can later add `actor` as sugar over the same lowering, and at that point it is an additive change.

Channels used to be presented as complementary to actors as well. They are now positioned simply as a stdlib type useful in producer/consumer patterns, and nothing about them requires special compiler support.

---

## Examples

### Single-threaded `async` (event-loop style)

A program with no `spawn` is just an event loop, and `await` is a coroutine yield.

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

- **Mutex / Lock / RwLock primitives.** These are wrappers over the platform's pthread primitives (`pthread_mutex_t`, `pthread_rwlock_t`). The async-aware suspending behaviour layers a wait queue on top, so the OS lock is held only across the actual critical section and never during the user-task's suspension.
- **Condition variable primitive.** This is a wrapper over `pthread_cond_t` with the same async-aware wait queue.
- **Fiber primitive.** `__cajeta_task_run(ctx, trampoline)` allocates a stackful fiber, with its own ~64 KB stack and `ucontext_t`, and queues it on the ready list. `__cajeta_task_wait(&task->done)` parks the current fiber if called from inside one, as a cooperative yield to the carrier, or `pthread_cond_wait`s on the global done-condvar if called from the main thread. `__cajeta_task_complete(&task->done)` flips the flag, broadcasts the done-condvar, and moves every parked fiber back to the ready queue so they can recheck their await conditions.
- **Executor.** There are N OS-thread carriers, each owning a Chase-Lev work-stealing deque (`cajeta_carrier_deque`) with atomic `top` / `bottom` sequence numbers over a fixed-size circular slot array, single-producer / multi-consumer. The owner side (`push_bottom` / `pop_bottom`) drains LIFO, which is the cache-warm end. The `steal` operation, FIFO from the deque's perspective, is what peer carriers call when their own deque empties. As of R8.3, N defaults to `min(_SC_NPROCESSORS_ONLN, 4)` and is overridden by `$CAJETA_CARRIERS` at the first `__cajeta_task_run`, clamped to `[1, CAJETA_MAX_CARRIERS=16]`. Owner-side pushes still take a per-carrier `deque_mutex` for the cross-thread case, where the main thread, a lock release, or a condvar notify pushes into a foreign deque, preserving Chase-Lev's single-producer contract under the mutex. The pool condvar (`__cajeta_task_queue_cond`) wakes idle carriers when work appears anywhere in the pool. The earlier multi-carrier deadlock is no longer reproducible across the full async/threading battery under N=4.
- **Cancellation marker.** Each fiber carries a `cancel_with` `void*`, the trigger Throwable, set by `__cajeta_fiber_cancel`. `__cajeta_task_wait` checks it on resume and re-raises (R5-C, shipped). There is no separate atomic token type.

The user-facing `cajeta.concurrent.*` classes (`Mutex`, `Lock`, `RwLock`, `Semaphore`, `Channel`, `Tasks`, the atomics) wrap these C helpers. The runtime entry points (`Cajeta.lockNew()` / `Cajeta.lockAcquire(p)` / `Cajeta.condvarWait(...)` / `Cajeta.rwlock*` / etc.) are an implementation detail, so user code should target the class API.

Suspension is delivered by `ucontext.h` (`getcontext` / `makecontext` / `swapcontext`). The compiler does not transform `async fn` bodies at all. They compile like ordinary functions, and the trampoline emitted at each spawn site wraps the call and runs inside the spawned fiber's stack.

---

## Interaction with the rest of the language

| Feature | Threading interaction |
|--|--|
| `#` transfer | The ownership-move marker doubles as the "send across threads" marker. Cross-thread transfer is what `#` already means at scope boundaries. |
| Templates | `Mutex<T>`, `RwLock<T>`, `Task<T>` monomorphize identically to class templates, with no erasure and primitives allowed. |
| Drop chain | `withLock`/`withWrite`/`withPermit` release on closure return or exception unwind (via an internal method-scoped `LockGuard`); `Lock.acquire` hands out a `LockGuard` whose drop releases. Task frame drop fires the scope-end drops in the right order. |
| Exceptions | Async exceptions raise on `await`; the setjmp/longjmp-based catch chain unwinds through the suspension stack. The lock primitives' internal guards drop on the unwind path, so unlocking-on-exception is automatic. |
| Method references | `obj::lockedMethod` works for methods that themselves use `Mutex<T>`, since nothing in the method reference itself is thread-specific. |

---

## Known gaps / open questions

- **Actor sugar.** This is deferred to v2+; see the dedicated section. The interesting design question is whether the keyword would generate a synthesized `Mutex<state>` wrapper class or take a different lowering, such as one OS thread per actor with a mailbox queue and a dispatcher.
- **Async iteration.** The `cajeta.concurrent.AsyncIterator<T>` interface has shipped (R9.7) with the canonical user-side loop pattern documented (`while ((opt = iter.next()).isPresent()) ...`). A `for (T x in iter) { ... }` desugaring over the same interface is the remaining v2.1 surface, and it is deferred. Multi-call iteration through an interface-typed receiver hits the M5(b) function-pointer/sret ripple gap and is tracked separately. Concrete-typed receivers loop cleanly today.
- **Pinning.** Some runtime objects, such as interop with OS callbacks, may need to be pinned to a thread. v1 has no pinning. Revisit this if FFI threading becomes a concern.
- **`runBlocking` escape hatch.** ✅ Shipped as `cajeta.concurrent.Tasks.runBlocking<R>(() -> R body) -> R`. The runtime's `__cajeta_task_wait` falls through to a condvar wait when the caller has no current fiber, so a plain non-async `main` can drive async work via `Tasks.runBlocking(() -> { ...await... })` without itself being `async`. The v1 surface covers primitive / value-return R via the spawn-of-lambda ABI, and heap-return callers keep using the explicit `await spawn body()` form.
- **Channel select.** ✅ Shipped as `cajeta.concurrent.Tasks.selectReceive<T>(Channel<T>[]) -> Optional<SelectResult<T>>` (R9.6). It returns a present `SelectResult` with the index and value of the channel that fired, and empty once every channel is closed and drained. The `select { case <- a; case <- b }` keyword form remains deferred, since the stdlib API covers the same multiplexed-receive use case.
- **Detached task ownership and leaks.** `detach` consumes captures by `#`. The detached task's lifetime is the runtime's lifetime, and nothing reclaims its result. Use it sparingly.

---

## Implementation status

Full runtime status lives in `docs/specification/concurrent/AsyncStatus.md`. As of
recent commits, R1 through R9 plus error-model v1 have shipped. The
runtime has stackful fibers on a carrier pool with per-carrier Chase–Lev
work-stealing deques (`CAJETA_CARRIERS=N`, default `min(nproc, 4)`). It is
multi-carrier by default, so spawned tasks run in parallel. Fibers are
pinned to their home carrier, `CAJETA_CARRIERS=1` forces single-carrier, and
the scheduler note under `async fn` has the detail. It also has explicit `scope { }` and implicit function-body scopes
with join-on-exit, R5-C cooperative cancellation, R5-D scope
exception-escalation, atomic ints (R8.1), Chase–Lev deques (R8.2), the
carrier pool plumbing (R8.3), a fiber-aware timer (R9.1, backing
`Duration` / `Fiber.sleep(Duration)` / `Tasks.withTimeout` / `Tasks.withDeadline`), and an
epoll-based async I/O reactor (R9.4).

The user-facing surface also gained spawn-of-lambda. `SpawnExpression`
accepts a function-typed scope field as the inner call, letting
stdlib utility methods such as `Tasks.withTimeout` spawn a callable
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

Surface classes `Fiber` and `Thread` have not been declared in
cajeta-source form yet. The runtime is shipped, and the wrappers are
designed. This is tracked in `specs/Features.md` as S-804.

Two V2 candidates are listed as known gaps but not yet scheduled, a
`for (T x in iter)` syntactic desugaring over `AsyncIterator<T>` and
`actor`-style sugar over the sync primitives. The `runBlocking` escape
hatch, `Channel.select` multiplex, and the `AsyncIterator<T>` contract
itself have shipped (R9.5 / R9.6 / R9.7; see `Tasks.runBlocking`,
`Tasks.selectReceive`, and `AsyncIterator`).
