# Cajeta Threading Model — Specification v1

## Goals

- **No data races at compile time.** The ownership/borrow machinery already used for single-threaded memory safety extends to thread boundaries; sharing mutable state without serialization is rejected before codegen.
- **Structured concurrency by default.** Every spawned task has a scope. Scopes don't return until their children complete or are cancelled. "Fire and forget" requires explicit opt-in.
- **No SAM interfaces for tasks.** Threading primitives consume function-typed values — there is no `Runnable`, no `Callable`, no `Future.get()` vs `CompletableFuture.then*` schism.
- **No `synchronized`, no `volatile`, no `ThreadLocal`.** Each of those Java warts comes from sharing mutable state across threads; Cajeta makes that the unusual case, not the default. Locks exist, but only via the RAII-guard primitives below — never as a separate "acquire / forget to release" API.
- **No GC, no runtime bootstrap.** Task state machines lower to plain structs allocated on the single-owner heap. The runtime is a small set of C functions (executor + scheduler + sync primitives), in the same style as the existing `__cajeta_*` helpers.

## Non-goals (v1)

- Preemptive cancellation. Cancellation is cooperative — a task observes a cancel signal at its next `await`.
- Custom executors beyond the built-in scheduler. (Today that scheduler is a single cooperative carrier; a work-stealing pool is planned, not shipped.)
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

Under the hood: each task is a **stackful fiber** — its own heap-allocated stack (~64 KB initial) plus a saved `ucontext`. A single carrier OS thread runs many fibers cooperatively; `await` parks the running fiber and switches back to the carrier, which picks another ready fiber. No `async fn` codegen transformation is required — fibers' suspension lives in the C runtime's context-switch primitive, and from the compiler's perspective an `async fn` is an ordinary function that happens to be invoked through a trampoline.

This is Java 21's virtual-thread model rather than the stackless state-machine model used by Rust/Swift/C#. We took stackful for two reasons: it ships much faster (no async-fn rewrite pass) and it removes function coloring — any function can call `await`, not just one declared `async`. The per-fiber stack cost is real but matches Java 21's tradeoff; if measurements ever demand it, stackless rewrite remains possible later without changing the surface syntax.

### 2. `scope { ... }` — structured concurrency blocks

`scope` is a block that owns the lifetime of every task spawned inside it. Control doesn't leave the block until every child task has finished or been cancelled.

```
async void processAll(List<Url> urls) {
    scope {
        for (Url u in urls) {
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
- A fresh allocator that's auto-promoted in transfer position (a bare `new T(...)` whose result has no prior identity, per *MemoryModel.md* § Borrow / transfer rules).

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

Cancellation is cooperative and lattice-shaped:

- Every `Task<T>` has a cancellation token, threaded implicitly.
- `await` checks the token on resume. If set, the await raises a `CancellationException`.
- A scope's parent task cancelling causes the scope to cancel all children, which causes their next `await` to raise.
- Cleanup happens via the standard `try` / `finally` machinery; the existing drop-chain machinery fires owners on the unwind.

The token is propagated automatically; user code rarely sees it explicitly. For explicit cancel-on-condition, `withTimeout(d) { ... }` and `withDeadline(t) { ... }` wrap a scope with a token that fires at the deadline.

---

## Synchronization primitives

All core sync primitives live in package **`cajeta.threading`** — `cajeta.threading.Mutex`, `cajeta.threading.Lock`, `cajeta.threading.RwLock`, `cajeta.threading.ConditionVariable`, and the guard types (`MutexGuard`, `LockGuard`, `ReadGuard`, `WriteGuard`). The future async runtime (`Task<T>`, scope/spawn/detach machinery) also lives there. Stdlib types built on these primitives (`Channel<T>`, `Semaphore`) are in the same package by convention. The code examples below omit the package qualifier for brevity, but the actual type names are fully qualified.

All core sync primitives use the **RAII guard pattern**: acquiring the lock returns a guard whose lifetime is the critical section. When the guard drops (scope exit, early return, exception unwind — all handled by the existing drop chain), the lock releases automatically. "Forgot to unlock" is structurally unrepresentable.

The primitives are async-aware: `await m.lock()` parks the current task on the wait queue and lets the executor run something else — it does not block the OS thread. OS-blocking forms are not exposed; if you find yourself wanting one, you almost certainly want to push the OS interaction into a dedicated worker task that owns the resource and serves async requests against it.

Implementation today (R4): each lock holds a pthread mutex (protecting its own `held` flag and wait queue) plus a per-lock pthread condvar (for any main-thread acquirer). When a fiber calls `lockAcquire` and the lock is held, the fiber links itself into the lock's wait queue and swaps back to the carrier — no OS-thread block. When `lockRelease` runs, it dequeues one waiter from the lock's queue and re-enqueues it on the carrier's ready queue, then signals the condvar to wake any main-thread waiter. Main-thread `lockAcquire` is a plain cond_wait (main is outside the fiber executor; OS-blocking is fine there).

### `Mutex<T>` — the fused-lock-and-data primitive

`Mutex<T>` owns the protected value. The only way to read or write it is through a guard obtained from `lock()` or `tryLock()`:

```
Mutex<int32> counter = new Mutex<int32>(0);

scope {
    spawn () -> async void {
        MutexGuard<int32> g = await counter.lock();
        g.value++;
        // g drops at end of statement → unlock
    };
}
```

API:

- `Mutex<T>(T initial)` — construct, owning `initial`.
- `await lock() → MutexGuard<T>` — async; suspends the task (not the OS thread) until acquired.
- `tryLock() → MutexGuard<T>?` — non-blocking; null when currently held by another task.

The guard exposes the protected value via a single `value` field; reads and writes flow through it. Drop releases.

**When to use `Mutex<T>`:** shared mutable state with one logical piece of data, where the lock and the data have the same scope. The fused pairing means a teammate adding another method that touches the value can't forget to lock — there's no `value` outside the lock.

### `Lock` — the no-data variant

When the section to gate is *not* tied to a single piece of data — sequencing two independent things, gating a side-effect-only block — `Lock` provides the same RAII shape without forcing a fake `Unit` wrapper:

```
Lock gate = new Lock();

public void doWork() {
    LockGuard g = await gate.acquire();
    sharedThing.step1();
    log.write("step1 done");
    sharedThing.step2();
    // g drops → release
}
```

API:

- `Lock()` — construct.
- `await acquire() → LockGuard` — async.
- `tryAcquire() → LockGuard?` — non-blocking.

**When to use `Lock`:** the critical section spans more than one logical value, or the gated thing is purely side-effects (e.g. an I/O sequence). Prefer `Mutex<T>` whenever there *is* a single `T` worth pairing — readers can tell the lock's intent from the type.

### `RwLock<T>` — read-heavy state

Many readers can hold a read guard concurrently; writers wait for outstanding reads to drain, then proceed exclusively:

```
RwLock<Config> cfg = new RwLock<Config>(loadInitial());

async Response handle(Request r) {
    ReadGuard<Config> g = await cfg.read();
    // Hundreds of concurrent requests can hold a read at once.
    return await process(r, g.value);
}

async void hotReload() {
    Config c = loadFresh();
    WriteGuard<Config> g = await cfg.write();
    g.value = c;
}
```

`ReadGuard<T>` and `WriteGuard<T>` are distinct types — you can't accidentally write through a read guard. The compiler enforces the distinction.

API:

- `RwLock<T>(T initial)` — construct.
- `await read() → ReadGuard<T>` — async; suspends until the writer (if any) releases.
- `await write() → WriteGuard<T>` — async; suspends until all readers drain.
- `tryRead() → ReadGuard<T>?` / `tryWrite() → WriteGuard<T>?` — non-blocking.

**When to use `RwLock<T>`:** read-very-heavy paths where serializing through a `Mutex<T>` would be a measurable bottleneck. Configs, caches with rare invalidation, read-mostly indexes. Reach for it only after measurement — for moderate contention `Mutex<T>` is simpler.

### `ConditionVariable` — wait / notify

Pairs with a `Mutex<T>` (or `Lock`) for the classic wait-for-condition pattern. `wait()` atomically unlocks the guard and suspends the task; `notify()` / `notifyAll()` wakes waiters, which then re-acquire before resuming:

```
Mutex<Queue<Item>> q = new Mutex<Queue<Item>>(new Queue<Item>());
ConditionVariable notEmpty = new ConditionVariable();

// Producer:
async void produce(Item item) {
    {
        MutexGuard<Queue<Item>> g = await q.lock();
        g.value.push(item);
    }
    notEmpty.notify();
}

// Consumer:
async Item consume() {
    MutexGuard<Queue<Item>> g = await q.lock();
    while (g.value.isEmpty()) {
        await notEmpty.wait(g);   // unlock, suspend, relock on wake
    }
    return g.value.pop();
}
```

API:

- `ConditionVariable()` — construct.
- `await wait(MutexGuard<T> g) → void` — atomically unlocks `g`, suspends; reacquires on wake before returning. `g` remains live and usable after the call.
- `notify()` — wake one waiter.
- `notifyAll()` — wake all waiters.

The `while` loop around `wait()` is the standard spurious-wakeup defense — `wait()` does not guarantee the condition is true on return.

### Decision tree

When you need to coordinate access to shared state:

1. **Is the state a single piece of data with one logical owner?** → `Mutex<T>`.
2. **Is the section to gate purely about ordering side-effects (no single value to pair the lock with)?** → `Lock`.
3. **Reads vastly dominate writes, and measurement shows `Mutex<T>` is the bottleneck?** → `RwLock<T>`.
4. **Need wait-for-condition (consumer-waits-for-producer style)?** → `Mutex<T>` (or `Lock`) plus `ConditionVariable`.
5. **Counted permit pool with overlapping lifetimes that no scope can bound?** → standard-library `Semaphore` (built on `Mutex<T>` + `ConditionVariable`).

Reach for the highest-numbered tool only when the lower-numbered ones don't fit. Each step down is a step toward subtler bugs and higher cognitive load.

---

## Standard library types (not language primitives)

The threading core stops at the four sync primitives above. The standard library provides higher-level building blocks composed from them — they don't need special compiler knowledge.

### `Channel<T>` — bounded MPMC queue

```
Channel<int32> ch = new Channel<int32>(capacity: 8);

scope {
    spawn () -> async void {
        for (int32 i = 0; i < 10; i++) {
            await ch.send(i);
        }
        ch.close();
    };
    spawn () -> async void {
        while (true) {
            int32 v = await ch.receive();
            // ...
        }
    };
}
```

A `Channel<T>` is internally a `Mutex<Queue<T>>` plus two `ConditionVariable`s (one for "not full", one for "not empty"). Bounded by default; `send` suspends when full, `receive` suspends when empty. `close` is one-way; subsequent sends raise, subsequent receives drain buffered items then raise on empty.

### `Semaphore` — counting permit pool

```
Semaphore s = new Semaphore(permits: 5);

async Result useResource() {
    SemaphorePermit p = await s.acquire();
    return await expensiveCall();
    // p drops at scope exit → release
}
```

Built on `Mutex<int32>` + `ConditionVariable`. Use case: bound the *number* of concurrent operations against an unbounded set of consumers (e.g. a server limiting global concurrency across all requests).

**Often you don't need a semaphore.** If the workload is statically batchable into "K at a time," chunked `scope { spawn N times }` expresses the limit in the program's shape with no permit bookkeeping. Reach for `Semaphore` when the consumers are unbounded and no scope can serve as the batching boundary.

### `AtomicInt32` / `AtomicInt64` — single-word atomics

```
AtomicInt32 hits = new AtomicInt32(0);

// any carrier
hits.fetchAdd(1);

// any carrier
int32 snapshot = hits.load();
```

Owns a heap-allocated word that compiler-emitted inline LLVM atomic
instructions operate against (`load atomic`, `store atomic`, `atomicrmw
add`, `cmpxchg`). All operations are sequentially consistent in v1;
explicit `MemoryOrder` overloads (`Relaxed` / `Acquire` / `Release` /
`AcqRel` / `SeqCst`) land in a follow-up.

API:
- `load() → T` — atomic read.
- `store(T v) → void` — atomic write.
- `fetchAdd(T delta) → T` — atomic add, returns the value the cell held
  before the add. Use for sequence numbers and counters.
- `compareAndSet(T expected, T desired) → boolean` — atomic CAS,
  returns whether the swap actually happened. Use as the lock-free
  retry-loop building block.

Backs the lock-free runtime data structures (Chase-Lev deque,
Treiber-style free lists) that the work-stealing pool — R8 § Executor —
needs to scale beyond a single carrier.

---

## Deferred: `actor` (v2+)

An earlier draft of this spec had `actor` as a third core primitive — a type kind with serialized state and an implicit mailbox. The decision for v1 is to **defer it** and revisit once `Mutex<T>` + the rest of the threading runtime have soaked. The actor pattern can already be expressed manually:

```
class Counter {
    private Mutex<int32> value = new Mutex<int32>(0);

    public async int32 next() {
        MutexGuard<int32> g = await value.lock();
        g.value++;
        return g.value;
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
    Mutex<int32> total = new Mutex<int32>(0);
    scope {
        for (Url u in urls) {
            spawn () -> async void {
                int32 s = await fetchScore(u);
                MutexGuard<int32> g = await total.lock();
                g.value += s;
            };
        }
    }
    MutexGuard<int32> g = await total.lock();
    return g.value;
}
```

### Bounded producer / consumer

```
async void pipeline(Source src, Sink sink) {
    Channel<Record> ch = new Channel<Record>(capacity: 64);
    scope {
        spawn () -> async void {
            while (Record r = await src.next()) {
                await ch.send(r);
            }
            ch.close();
        };
        spawn () -> async void {
            while (true) {
                Record r = await ch.receive();
                await sink.write(r);
            }
        };
    }
}
```

### Timeout

```
async Response getWithTimeout(Url u, Duration d) {
    return await withTimeout(d) {
        await http.get(u);
    };
}
```

---

## Runtime requirements

A small C runtime ships in `runtime/native/`, paralleling the existing exception / drop-chain helpers:

- **Mutex / Lock / RwLock primitives** — wrappers over the platform's pthread primitives (`pthread_mutex_t`, `pthread_rwlock_t`). The async-aware suspending behaviour layers a wait queue on top — the OS lock is held only across the actual critical section, not during the user-task's suspension.
- **Condition variable primitive** — wrapper over `pthread_cond_t` with the same async-aware wait queue.
- **Fiber primitive** — `__cajeta_task_run(ctx, trampoline)` allocates a stackful fiber (its own ~64 KB stack + `ucontext_t`) and queues it on the ready list. `__cajeta_task_wait(&task->done)` parks the current fiber if called from inside one (cooperative yield to the carrier), or `pthread_cond_wait`s on the global done-condvar if called from the main thread. `__cajeta_task_complete(&task->done)` flips the flag, broadcasts the done-condvar, and moves every parked fiber back to the ready queue so they can recheck their await conditions.
- **Executor** — currently a single carrier OS thread. A pool with work-stealing is a future enhancement; the cooperative model means correctness doesn't depend on parallelism, only on the carrier draining the ready queue.
- **Token primitive** — atomic flag for cancellation; tasks check on resume. (Not yet implemented; lands with R5.)

The user-facing `cajeta.threading.*` classes wrap these C helpers — the runtime entry points are an implementation detail. Today's intrinsic-level path (`Cajeta.lockNew()` / `Cajeta.lockAcquire(p)` / etc.) is a transitional bootstrap that the future `cajeta.threading.Lock` class will subsume; user code should target the class API, not the intrinsics, once they exist.

Suspension is delivered by `ucontext.h` (`getcontext` / `makecontext` / `swapcontext`). The compiler doesn't transform `async fn` bodies at all — they compile like ordinary functions; the trampoline emitted at each spawn site wraps the call and runs inside the spawned fiber's stack.

---

## Interaction with the rest of the language

| Feature | Threading interaction |
|--|--|
| `#` transfer | The ownership-move marker doubles as the "send across threads" marker. Cross-thread transfer is what `#` already means at scope boundaries. |
| Templates | `Mutex<T>`, `RwLock<T>`, `Task<T>` monomorphize identically to class templates — no erasure, primitives allowed. |
| Drop chain | Guard drop releases the lock — same RAII pattern the rest of the memory model uses. Task frame drop fires the scope-end drops in the right order. |
| Exceptions | Async exceptions raise on `await`; the existing setjmp/longjmp-based catch chain unwinds through the suspension stack. Lock guards drop on the unwind path, so unlocking-on-exception is automatic. |
| Method references | `obj::lockedMethod` works for methods that themselves use `Mutex<T>` — nothing thread-specific in the method reference itself. |

---

## Known gaps / open questions

- **Actor sugar.** Deferred to v2+; see the dedicated section. The interesting design question is whether the keyword would generate a synthesized `Mutex<state>` wrapper class or take a different lowering (e.g. one OS thread per actor, mailbox queue, dispatcher).
- **Async iteration.** `for (T x in asyncIterable) { ... }` syntax. Deferred — requires designing `AsyncIterator<T>` and the desugaring rule.
- **Pinning.** Some runtime objects (e.g. interop with OS callbacks) may need to be pinned to a thread. v1 has no pinning; revisit if FFI threading becomes a concern.
- **`runBlocking` escape hatch.** Today `await` is illegal outside `async` functions / `spawn` bodies. A Kotlin-style `runBlocking { ... }` for letting non-async code call async is open for v2.
- **Channel select.** Multiplexed `select { case <- a: ...; case <- b: ... }` syntax — deferred.
- **Detached task ownership and leaks.** `detach` consumes captures by `#`. The detached task's lifetime is the runtime's lifetime; nothing reclaims its result. Use sparingly.

---

## Implementation status

Full runtime status lives in `cajeta-docs/stdlib/AsyncStatus.md`. As of
recent commits, R1 through R5-D plus error-model v1 have shipped:
stackful fibers, a **single-carrier cooperative scheduler**, explicit
`scope { }` + implicit function-body scopes with join-on-exit, R5-C
cooperative cancellation, and R5-D scope exception-escalation.

**NOT YET shipped** (despite earlier drafts of this section claiming
otherwise): a work-stealing multi-carrier pool, a timer wheel
(`withTimeout`/`withDeadline`), and an async I/O reactor / netpoller.
Today a single carrier OS thread runs all fibers cooperatively, and any
blocking syscall inside a fiber blocks every fiber. Those three are the
planned R8/R9 work.

## Pinning tests

| Surface | Tests |
| ------- | ----- |
| `Task<T>` typing | `test/parser/TaskTypingTests.cpp` |
| `spawn` syntax + drop semantics | `test/parser/SpawnDropTests.cpp`, `test/parser/AsyncSyntaxTests.cpp` |
| `detach` syntax | `test/parser/DetachTests.cpp` |
| Per-fiber drop chain | `test/parser/PerFiberDropChainTests.cpp` |
| `Lock` class + intrinsics | `test/parser/LockIntrinsicTests.cpp`, `test/parser/LockClassTests.cpp` |

## Open items

Surface classes `Fiber` and `Thread` haven't been declared in
cajeta-source form yet (the runtime is shipped; the wrappers are
designed). Tracked in `Features.md` as S-804.
