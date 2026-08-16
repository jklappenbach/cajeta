---
id: concurrent-locks
applies-to: [cajeta/concurrent/Lock, cajeta/concurrent/LockGuard]
title: Lock + LockGuard — the no-data RAII mutex gate
description: Acquire a Lock to guard a critical region; the returned #LockGuard releases on drop, or use tryAcquire/releaseLock manually.
---

# Lock + LockGuard — the RAII mutex gate

Use this pair to guard a critical region that protects state **not** bundled into one
value. `Lock` is the access point; `LockGuard` is the held-token whose drop releases.
You instantiate `Lock`; you never construct `LockGuard` yourself — you receive it from
`Lock.acquire()`. If your lock protects a single piece of data, use `Mutex`
(`cajeta/concurrent/Mutex`) instead so the lock and data are fused; for reader/writer
splits use `RwLock` + `WriteGuard` (`cajeta/concurrent/RwLock`).

## Members and roles
- **`Lock`** — the access point. Wraps the async-aware lock intrinsics (`Cajeta.lock*`).
  Construct one with `heap Lock()`. Holds a raw `pointer handle`; its destructor calls
  `Cajeta.lockDestroy`.
- **`LockGuard`** — the RAII held-token. Holds the raw `handle` (not a back-reference to
  the `Lock`); its destructor calls `Cajeta.lockRelease(handle)`. "Forgot to unlock" is
  structurally unrepresentable.

## Two ways in — pick one, don't mix on a single acquisition
- **Blocking, RAII (preferred):** `acquire()` blocks until held, then returns a
  `#LockGuard`. Release is automatic on the guard's drop. Do **not** call `releaseLock()`
  for a lock taken this way.
- **Non-blocking, manual:** `tryAcquire()` returns `int32` — `1` = you now hold it
  (no guard is produced), `0` = already held by someone else. On a `1`, you own release
  and must pair it with exactly one `releaseLock()`.

## Object graph and ownership across the boundary
`acquire()` calls `Cajeta.lockAcquire(handle)` then `return heap LockGuard(this.handle)`.
The guard is **heap-owned and transferred to you** (`#LockGuard`) and shares the `Lock`'s
raw handle. Lifetime contract: **guard <= lock** — never let a `LockGuard` outlive its
`Lock` (the `Lock` destructor destroys the underlying mutex the guard still points at).
Bind the guard to a name in the scope you want protected; discarding the return value
drops it immediately and releases the lock at once.

## Worked example
```cajeta
import cajeta.concurrent.Lock;
import cajeta.concurrent.LockGuard;

public class Counter {
    public static void critical() {
        Lock gate = heap Lock();

        // RAII path: held until the guard drops.
        LockGuard g #= gate.acquire();
        // ... critical section runs while g is alive ...
        // g drops on method return -> lock released, THEN gate is destroyed.

        // Manual path: no guard, you release.
        if (gate.tryAcquire() == 1) {
            // ... work while holding the lock ...
            gate.releaseLock();
        }
    }
}
```

## Sharp edges
- **Drop is method-scoped, not block-scoped.** A `#LockGuard` declared inside an inner
  `{ }` does **not** release at the closing brace — it releases when the enclosing
  *method* returns. To bound a critical section more tightly than a method, factor it
  into its own method (the guard drops on that method's return) or use the manual
  `tryAcquire`/`releaseLock` pair.
- **Never `releaseLock()` a guard-held lock,** and never double-release a `tryAcquire`.
  Match `acquire` with the guard's drop, and each `tryAcquire()==1` with one
  `releaseLock()`.
- **No data is stored here.** `Lock` guards a region, not a value — there is no
  `get()`/`set()`. Reach for `Mutex` when you want the lock fused with one datum.

## Async behaviour
On the cooperative single-carrier scheduler a fiber that hits a held lock parks and
yields the carrier (does not block the OS thread); the main thread blocks on the lock's
condvar. See `docs/specification/concurrent/AsyncStatus.md`.
