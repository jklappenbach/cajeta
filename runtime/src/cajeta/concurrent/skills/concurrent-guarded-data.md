---
id: concurrent-guarded-data
applies-to: [cajeta/concurrent/Mutex, cajeta/concurrent/RwLock, cajeta/concurrent/WriteGuard]
title: Mutex<T> / RwLock<T> — lock fused with the data it protects
description: Guard one piece of data with a lock by operating on it only inside withLock/withWrite closures; pick Mutex vs RwLock.
---

# Guarded data: `Mutex<T>` and `RwLock<T>`

Use these when you have **one piece of data** and want the lock and the data to be
inseparable: the value is owned by the container and is only reachable inside a closure
that runs while the lock is held. "Read or write the data without the lock" is
structurally unrepresentable — there is no exposed `value` handle that escapes the
locked region.

- **`Mutex<T>`** — mutual exclusion. One holder at a time. Reach for it by default.
- **`RwLock<T>`** — many shared readers OR one exclusive writer, writer-preference.
  Use it **only when reads vastly dominate writes**. (Note: under the current
  scheduler there is no wall-clock read parallelism yet; the shared-read *semantics*
  hold but the throughput win lands with the work-stealing pool.)
- **`WriteGuard`** — `RwLock`'s internal drop-to-release guard for the write lock. You
  do **not** construct it; `withWrite` holds one for you.

If your critical region protects state that is **not** a single value, use `Lock` +
`LockGuard` instead (see `cajeta/concurrent/Lock`).

## Members and roles

| Type | Role | You instantiate? |
|------|------|------------------|
| `Mutex<T>` | owns `T`, gates all access behind the lock | yes (`heap Mutex<T>(initial)`) |
| `RwLock<T>` | owns `T`, splits shared read / exclusive write | yes (`heap RwLock<T>(initial)`) |
| `WriteGuard` | RAII guard; drop releases `RwLock`'s write lock | no — `withWrite` owns it |

`Mutex`'s exclusive critical sections are guarded internally by a method-scoped
`LockGuard` (from `cajeta/concurrent/Lock`), the same drop-to-release pattern as
`WriteGuard`.

## Worked example (with imports)

```cajeta
import cajeta.concurrent.Mutex;
import cajeta.concurrent.RwLock;

// Mutex<T>: the closure passed to withLock IS the critical section; its
// return value is stored back as the new protected value.
Mutex<int32> m = heap Mutex<int32>(0);
m.withLock((int32 v) -> v + 5);     // value = 5, under the lock
m.withLock((int32 v) -> v + 37);    // value = 42
int32 now = m.get();                // snapshot under the lock -> 42

// Block until a predicate holds, then act atomically:
m.withLockWhen((int32 v) -> v >= 42, (int32 v) -> 0);  // wait for >=42, reset to 0

// RwLock<T>: read() takes the shared lock and copies out; withWrite takes
// the exclusive lock and stores the closure's result.
RwLock<int32> rw = heap RwLock<int32>(5);
rw.withWrite((int32 v) -> v + 9);   // exclusive: value = 14
int32 snapshot = rw.read();         // shared read lock -> copy of 14
```

## Call surface (v1)

- `Mutex.withLock((T) -> #T fn)` — acquire, `value = fn(value)`, wake `withLockWhen`
  waiters, release. The closure's return is `#T` (moved into the container).
- `Mutex.withLockWhen((T) -> boolean cond, (T) -> #T fn)` — acquire, then loop-wait
  (atomically release/park/reacquire) until `cond(value)` holds, then store `fn(value)`.
  `cond` **must be side-effect-free** — it is re-evaluated on every wake.
- `Mutex.get() -> T` — snapshot copy under the lock.
- `RwLock.read() -> T` — snapshot copy under a shared read lock.
- `RwLock.withWrite((T) -> #T fn) -> void` — exclusive mutate-and-store.

Not present yet: a compute-only read variant (`withRead((T) -> R)`), a non-blocking
`tryLock` on `Mutex`. Don't reach for them; use `withLock`/`read`.

## Ownership and lifecycle (the part that bites)

- **Construction takes ownership**: `Mutex(#T initial)` / `RwLock(#T initial)` move
  `initial` into the container. After `heap Mutex<T>(x)`, the container owns the value.
- **Closure return is `#T`**: the value `fn` returns is moved back into the container as
  the new protected value. Build the new value inside the closure; do not stash a
  reference to the old one.
- **The value must not escape.** For value/primitive `T`, `get()`/`read()` return an
  independent copy — safe. For a **heap class `T`, `get()`/`read()` hand out a reference
  that outlives the lock** — that defeats the guarantee. For mutable/class `T`, do all
  work inside `withLock`/`withWrite` and never return the live value out of the closure.
- **Auto-release on every exit.** `withLock`/`withLockWhen` hold a method-scoped
  `LockGuard`; `withWrite` holds a method-scoped `WriteGuard`. Their drop releases the
  lock on normal return **and on exception unwind through the frame** — a throw inside
  `fn` still releases. "Forgot to unlock" cannot happen.
- **No manual unlock here.** Unlike `Lock` (which has `tryAcquire`/`releaseLock`), these
  containers expose no acquire/release; the lock lifetime is exactly the closure's
  duration. `WriteGuard`'s contract is "guard <= lock"; you never construct or release
  it yourself.
- **Container disposal**: `~Mutex`/`~RwLock` destroy the underlying OS primitives;
  `Mutex` also owns a condvar (for `withLockWhen`). Let the container drop normally.

## Async behaviour

Acquiring a held lock from a fiber **parks the fiber and yields to its carrier** rather
than blocking the OS thread; the wait queue is guarded by a pthread mutex so it is
correct across carriers. So blocking in `withLockWhen` / a contended `withLock` is
cooperative, not a thread stall.
