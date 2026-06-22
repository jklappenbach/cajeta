---
id: concurrent-Lock-tryAcquire
applies-to: [cajeta/concurrent/Lock.tryAcquire]
title: Lock.tryAcquire — non-blocking acquire, manual release
description: tryAcquire returns 1 (taken) or 0 (already held), produces no LockGuard; a 1 must be paired with exactly one releaseLock().
---

# Lock.tryAcquire — the non-blocking, manual-release path

`public int32 tryAcquire()` — attempts to take the lock **without blocking**. Returns
`1` if you now hold it, `0` if it was already held by someone else. This is the manual
counterpart to `acquire()`: read it when you must not block, or when you need a critical
section bounded more tightly than the enclosing method (where `acquire()`'s method-scoped
`#LockGuard` drop is too coarse). For `Lock`/`LockGuard` orientation and the
blocking path, see `cajeta/concurrent/Lock` (skill `concurrent-locks`).

## Return semantics — the sharp edge
- **`1` = acquired.** You now hold the lock and you own its release. There is **no
  `LockGuard`** — nothing drops to release for you. You **must** call `releaseLock()`
  exactly once before the path leaves the held region.
- **`0` = already held** by another holder. You do **not** hold the lock; do **not**
  call `releaseLock()` (that would release a lock you never took).

It is a plain `int32` sentinel, not a guard and not nullable; check `== 1` explicitly.

## Required call sequence
1. `int32 got = lock.tryAcquire();`
2. branch on `got == 1`;
3. on `1` only: do the work, then `lock.releaseLock()` exactly once.

Never mix the two acquisition styles on one acquisition: a `tryAcquire()==1` is released
by `releaseLock()`, never by a `LockGuard`; a lock taken via `acquire()` is released by
its guard's drop and must **not** be `releaseLock()`d.

## Failure modes
- No exception is raised — contention is reported as the `0` return, not thrown.
- Double-release (calling `releaseLock()` after a `0`, or twice after one `1`) releases a
  lock you don't own — a latent corruption bug, not a caught error.

## Side effects
On `1`, mutates the underlying mutex state to held (via `Cajeta.lockTryAcquire(handle)`).
On `0`, no state change. Does not spawn, does not touch the filesystem.

## Async behaviour
`tryAcquire()` does **not** park or yield — it returns immediately either way. (Contrast
`acquire()`, which parks the fiber / blocks the main thread on contention.)

## Example
Mirrors `test/parser/LockClassTests.cpp` (`tryAcquireUncontendedSucceeds`):

```cajeta
import cajeta.concurrent.Lock;

public class Probe {
    public static void run() {
        Lock lock = heap Lock();
        int32 got = lock.tryAcquire();
        if (got == 1) {
            // ... work while holding the lock ...
            lock.releaseLock();      // pair the successful try with exactly one release
        }
        // got == 0: lock was already held — do not release.
    }
}
```
