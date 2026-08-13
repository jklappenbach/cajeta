---
id: concurrent-Mutex
applies-to: [cajeta/concurrent/Mutex]
title: Mutex<T> — lock fused with the data it protects
description: Use withLock/withLockWhen closures to mutate a lock-owned T; the data never escapes the lock.
---

# Mutex<T> — the fused lock + protected value

**Access point.** Reach for `Mutex<T>` when one piece of data must only ever be
touched under a lock. The mutex *holds* a single `T` and the only way to read or
write it is to hand a closure to `withLock` / `withLockWhen` — that closure *is*
the critical section (the Java `synchronized (obj) { ... }` shape). "Touch the
data without the lock" is structurally unrepresentable: there is no escaping
`value` handle. Use a bare `cajeta/concurrent/Lock` instead when the protected
state is *not* a single value.

## Construct & ownership

```cajeta
import cajeta.concurrent.Mutex;

Mutex<int32> counter = heap Mutex<int32>(0);   // primitive: copied in
Mutex<Session> s = heap Mutex<Session>(#sess); // class T: title moves into the mutex
```

`Mutex(T initial)` — the formal is plain, so the caller decides the mode. The
constructor stores with `#=`, which carries whatever arrived: `heap Mutex<T>(#v)`
moves the title in and the mutex owns (and drops) the value; `heap Mutex<T>(v)`
stores a borrow, and the caller keeps title and must outlive the mutex. Either
way, gate all later access through the lock. The mutex holds its own native
lock + condvar handles; its destructor (`~Mutex`) frees both at scope/drop exit,
so you never close it manually.

## The methods that matter

- `void withLock((T) -> #T fn)` — acquire, pass the current value to `fn`, store
  what `fn` returns (`#T`: ownership of the result moves into the mutex), release.
  After storing it wakes every `withLockWhen` waiter to re-check its predicate.
- `void withLockWhen((T) -> boolean cond, (T) -> #T fn)` — acquire, then block
  until `cond(value)` holds (each wait atomically releases, parks/cond-waits, and
  reacquires), then run `fn`, store, and wake other waiters. `cond` **must be
  side-effect-free** — it is re-evaluated on every wake.
- `T get()` — snapshot the value under the lock and return a copy. See the caveat
  below.

```cajeta
counter.withLock((int32 v) -> v + 5);                  // critical section: +5
counter.withLockWhen((int32 v) -> v >= 5, (int32 v) -> 0);  // wait for >=5, reset
int32 now = counter.get();                             // -> 0
```

## get() caveat — value T only

`get()` returns an independent copy and is meant for value / primitive `T`. For a
**heap class `T`** the snapshot is a reference that outlives the lock — mutating it
afterward is an unsynchronized data race. To operate on a mutable/heap `T` safely,
do the work *inside* `withLock` instead of pulling it out with `get()`.

## Lifecycle, exceptions, concurrency

- **No manual unlock.** `withLock`/`withLockWhen` release via a method-scoped
  `cajeta/concurrent/LockGuard` whose drop fires on normal return **and on
  exception unwind** — a throw inside `fn` still releases the lock. Forgetting to
  unlock is impossible.
- **Async-aware.** Acquiring a held lock from a fiber parks the fiber and yields
  to its carrier rather than blocking the OS thread (multi-carrier work-stealing
  pool, `CAJETA_CARRIERS`, default `min(nproc, 4)`); the wait queue is guarded by a
  pthread mutex so it is correct across carriers. `withLockWhen` is the standard
  fiber producer/consumer wait.

## What it does NOT provide (v1)

No compute-only `(T) -> R` read variant and no non-blocking `tryLock` — those land
in a later sub-phase. For a non-blocking attempt or lock-without-fused-data use
`cajeta/concurrent/Lock` (`tryAcquire()`). There is no method to borrow `value`
out of the lock; route mutation through `withLock`.

## Producer/consumer across fibers (idiomatic)

```cajeta
import cajeta.concurrent.Mutex;

// consumer parks until the value reaches 10, then consumes
public static async int32 consumer(Mutex<int32> m) {
    m.withLockWhen((int32 v) -> v >= 10, (int32 v) -> v + 1);
    return m.get();
}
public static async int32 producer(Mutex<int32> m) {
    m.withLock((int32 v) -> 10);   // publishes -> wakes the consumer's predicate
    return 0;
}
```
