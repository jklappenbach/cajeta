---
id: concurrent-Semaphore
applies-to: [cajeta/concurrent/Semaphore]
title: Semaphore — counting permit pool bounding concurrent access
description: Bound concurrent operations with Semaphore — withPermit (preferred) or acquire/release, when to reach for it over a chunked scope, and the advisory availablePermits snapshot.
---

# Semaphore — counting permit pool

Bound the number of fibers in a critical region or hitting a rate-limited resource
to at most `K` at once. `Semaphore` is an **entry-point** type: you construct it with
a permit count and call it directly. `acquire` blocks (parks the fiber) until a permit
is free then takes one; `release` returns one and wakes a waiter.

**Reach for it only when consumers are unbounded.** If the work is statically batchable
into "K at a time", a chunked `scope { spawn K times }` expresses the limit in the
program's shape — prefer that. Use `Semaphore` when no scope can serve as the batching
boundary (e.g. an open-ended stream of requests sharing a fixed connection budget).

## Construct, then guard a region with withPermit

```cajeta
import cajeta.concurrent.Semaphore;

stack gate = Semaphore(4);            // 4 permits up front: at most 4 concurrent holders

// Preferred: scoped acquire/release that survives a throw.
gate.withPermit(() -> {
    callRateLimitedApi();
});
```

`Semaphore(int32 initial)` seeds the pool with `initial` free permits — the cap on
concurrent holders. `Semaphore(0)` is valid and useful as a pure hand-off latch
(consumer `acquire`s and parks, producer `release`s to wake it).

## The whole surface

- `withPermit(() -> void fn)` — **preferred.** `acquire`, run `fn`, `release` in a
  `finally` so the permit is returned even if `fn` throws. The permit is held for the
  whole of `fn` (the bounded operation), not just the brief count update.
- `acquire() -> void` — take one permit, blocking until one is free.
- `release() -> void` — return one permit, waking a waiter to re-check.
- `availablePermits() -> int32` — current free-permit count.

Use `withPermit` unless control flow forbids a closure. Only drop to the manual pair
when you must — and then pair every `acquire` with a `release` in a `finally`:

```cajeta
gate.acquire();
try {
    callRateLimitedApi();
} finally {
    gate.release();
}
```

## availablePermits is a snapshot, not a reservation

`availablePermits()` returns the count under the lock, but the value is **advisory**
the instant it returns — another fiber can take or return a permit before you act on it.
**Never branch on it to decide whether to acquire** (that is a race); just call
`acquire`/`withPermit` and let blocking do the gating. It is for diagnostics, logging,
and tests (e.g. asserting the count rebalances after `withPermit`).

## Ownership, lifecycle, concurrency

- **`fn` is borrowed** — `withPermit` calls it and returns; it takes no ownership and
  no `#`. Same for the `initial` primitive arg to the constructor.
- **No `close()`/`dispose()`.** The destructor releases the underlying lock + condvar
  intrinsics on normal cajeta drop. Construct `stack` for scope-bound lifetime, or
  `heap` to share across spawned fibers (tests pass a `heap Semaphore(...)` into
  `spawn`ed functions).
- **Shared and fiber-safe by design** — that is the point. All count mutation happens
  under the internal lock; safe to call from any fiber concurrently. Reusable
  indefinitely.

## What it does NOT do

- **Not a binary mutex** — to protect one critical region or piece of data use `Lock`
  (`cajeta/concurrent/Lock`) or `Mutex` (`cajeta/concurrent/Mutex`); a `Semaphore(1)`
  is not the idiomatic mutex and gives no RAII guard.
- **No guard object / RAII token** — unlike `Lock.acquire`, `acquire` returns `void`
  and hands you nothing to drop; you must `release` yourself (or use `withPermit`).
- **No `tryAcquire`, no timeout, no permit count argument** to `acquire`/`release` —
  exactly one permit per call, and `acquire` always blocks.
- **No fairness guarantee** — `release` does `notifyAll` and each waiter re-checks the
  count, so wakeups are not FIFO-ordered.
- **No upper-bound enforcement** — `release` increments unconditionally; over-releasing
  (more releases than acquires) silently raises the permit ceiling above `initial`.

## Blocking hand-off example (from the tests)

```cajeta
import cajeta.concurrent.Semaphore;

public static async int32 consumer(Semaphore s, int32[] out) {
    s.acquire();          // parks: starts at 0 permits
    out[0] = 42;
    return 0;
}
public static async int32 producer(Semaphore s) {
    s.release();          // wakes the consumer
    return 0;
}
public static int32 run() {
    Semaphore s = heap Semaphore(0);
    int32[] out = heap int32[1];
    scope {
        spawn consumer(s, out);
        spawn producer(s);
    }
    return out[0];        // 42
}
```

Built on the same lock + condition-variable intrinsics as `Mutex`
(`cajeta/concurrent/Mutex`).
