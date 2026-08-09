---
id: concurrent-overview
applies-to: [cajeta.concurrent]
title: cajeta.concurrent — concurrency library orientation & routing
description: Pick the right concurrency primitive (Lock/Mutex/RwLock/Semaphore/Channel/Atomics/Tasks) and apply the library-wide fiber, ownership, and error rules.
---

# cajeta.concurrent — orientation & routing

Structured concurrency over a **fiber/carrier** scheduler. You write `async`
methods, `spawn` them inside a `scope { ... }` (the scope joins all children at
its closing brace), and synchronize with the primitives below. A blocked
primitive **parks the fiber and yields its carrier** — it does not block the OS
thread — so thousands of fibers share a small carrier pool (`CAJETA_CARRIERS`,
default `min(nproc, 4)`).

## Task → primitive

| You want to… | Use |
|---|---|
| Guard a critical region (state not bundled into one value) | `Lock` → `acquire()` returns a `#LockGuard`; drop releases |
| Guard **one piece of data** so it can't be touched unlocked | `Mutex<T>` → `withLock`, `withLockWhen`, `get` |
| Read-heavy shared data (many readers, rare writer) | `RwLock<T>` → `read`, `withWrite` |
| Cap concurrency to N permits over unbounded consumers | `Semaphore` → `withPermit`, or `acquire`/`release` |
| Hand items between fibers (bounded MPMC queue) | `Channel<T>` → `send`, `receive`, `close` |
| Lock-free counter / sequence / id | `AtomicInt32`, `AtomicInt64` |
| Wait on a spawned task with a deadline | `Tasks.withTimeout(d, t)` / `withDeadline` |
| Run async work from non-async code (sync→async bridge) | `Tasks.runBlocking(() -> body())` |
| Multiplex receive across many channels (Go-style select) | `Tasks.selectReceive(channels)` |
| Ambient per-request state (sound `ThreadLocal` replacement) | `FiberLocal<T>` → `where` + `get`/`orElse` |
| Carry that state across an unstructured handoff (channel/detach) | `FiberContext.capture()` then `ctx.run(...)` |
| Consume an async stream | `AsyncIterator<T>` (`Channel<T>` is the canonical source) |

**Not provided here:** raw OS threads (you `spawn` fibers, not threads); a
condition-variable type (condvars are an intrinsic, exposed only via `Mutex`'s
`withLockWhen`); cancellation of a pure CPU loop (cooperative cancel only fires
at a yield point — `await`, channel receive, `Lock.acquire`, etc.).

## Cross-cutting invariants

- **Drop-chain releases guards.** `acquire()`/`withLock`/`withWrite` produce a
  scoped `LockGuard`/`WriteGuard` whose destructor releases the lock. Release
  happens on normal return **and** on exception unwind — "forgot to unlock" is
  unrepresentable. Corollary: bind the guard to a scope; discarding the
  `#LockGuard` from `Lock.acquire()` releases the lock immediately.
- **`tryAcquire()` is the exception.** A successful `Lock.tryAcquire()` (returns
  `1`) gives you **no guard** — you must call `releaseLock()` yourself. `0` means
  already held. Never `releaseLock()` a lock taken via `acquire()`.
- **`#` at the CALL SITE transfers ownership of guarded values.** `Mutex(T initial)`,
  `RwLock(T initial)` and `SelectResult(int32 index, T value)` declare plain formals
  and store with `#=`, so transfer is the caller's opt-in: `heap Mutex<T>(#v)` moves
  the value in, `heap Mutex<T>(v)` stores a borrow. (`FiberContext.capture()` does
  return a `#FiberContext` — that is a return, not a formal.) The protected value
  never escapes its locked region — there is no `value` handle; you operate on it
  only inside the `withLock`/`withWrite` closure.
- **Returns are stack `Optional<T>`, not null.** `Channel.receive`/`tryReceive`,
  `Tasks.withTimeout`/`selectReceive` return a **stack** `Optional` (no per-item
  heap alloc): present = value, empty = terminal (closed-and-drained / timeout /
  all-channels-done). Check `isPresent()` before `get()`.
- **Errors throw; they are not error codes.** `Channel.send` to a closed channel
  throws; `Mutex`/`FiberLocal.get` on an unbound fiber throws. There is no
  `Result` type — use try/catch, or the `Optional`-returning variants that
  consume the throw for you (e.g. `withTimeout` swallows the cancellation
  sentinel).
- **v1 targets value/primitive `T`.** For a heap-class `T`, `Mutex.get`/`read`
  hand out a reference that outlives the lock, and `Channel` lends its slots —
  it never drops a buffered item, so the sender must keep the item alive until
  a receiver takes it (drain before the *producer's* scope ends).

## Canonical example

```cajeta
import cajeta.concurrent.Mutex;

public static async int32 worker(Mutex<int32> m) {
    m.withLock((int32 v) -> v + 1);   // critical section; result stored back
    return 0;
}

public static int32 run() {
    Mutex<int32> counter = heap Mutex<int32>(0);
    scope {                            // joins all spawned children at `}`
        spawn worker(counter);
        spawn worker(counter);
    }
    return counter.get();              // 2 — snapshot under the lock
}
```

Channel hand-off (stack `Optional` drain loop):

```cajeta
import cajeta.lang.Optional;
import cajeta.concurrent.Channel;

Channel<int32> ch = heap Channel<int32>(2);
ch.send(1); ch.send(2); ch.close();   // close is one-way, idempotent
stack Optional<int32> x = ch.receive();   // present 1 (drains after close)
while (x.isPresent()) { use(x.get()); x = ch.receive(); }  // empty => done
```

## Disambiguation

- **Lock vs Mutex** — `Lock` is a no-data RAII gate for state not fused into one
  value; `Mutex<T>` holds the data and only exposes it through a locked closure.
  Prefer `Mutex` when there is exactly one piece of data to protect.
- **Mutex vs RwLock** — both hold their `T` (owning it only when the caller
  transferred with `#`, per the call-site rule above); `RwLock` adds shared
  reads. Reach for `RwLock` only when reads vastly dominate writes
  (writer-preference avoids writer starvation).
- **Semaphore vs scope** — if work is statically "K at a time", a chunked
  `scope { spawn K times }` expresses the limit structurally; use `Semaphore`
  only when consumers are unbounded and no scope bounds them.
- **Atomics vs Lock/Mutex** — atomics for a single lock-free integer cell
  (counters, ids, CAS); a lock once you need to make two updates consistent.
  Default ops are seq_cst; weaker orderings are separate named methods
  (`loadAcquire`, `storeRelease`, …), not a parameter.
- **Channel vs Mutex** — `Channel` hands items between fibers with backpressure
  (producer/consumer), lending its slots rather than taking title — never write
  `send(#item)`; `Mutex` shares in-place mutable state.

## Setup

Import per type: `import cajeta.concurrent.Mutex;` etc. (`Optional` lives in
`cajeta.lang`, `Duration` in `cajeta.time`). Primitives are built on the
runtime lock/condvar/atomic intrinsics (`Cajeta.lock*`, `Cajeta.condvar*`,
`Cajeta.atomic*`); the scheduler honors `CAJETA_CARRIERS`.

## Downward pointers

Each type carries its own detail in its source file's doc comment under
`runtime/src/cajeta/concurrent/` — `Mutex`, `RwLock`, `Lock`/`LockGuard`,
`WriteGuard`, `Semaphore`, `Channel`/`SelectResult`, `AtomicInt32`/`AtomicInt64`,
`Tasks`, `FiberLocal`/`FiberLocalBox`/`FiberContext`, `AsyncIterator`.
