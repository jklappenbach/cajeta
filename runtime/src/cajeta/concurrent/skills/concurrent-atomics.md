---
id: concurrent-atomics
applies-to: [cajeta/concurrent/AtomicInt32, cajeta/concurrent/AtomicInt64]
title: Atomic integer cells (AtomicInt32 / AtomicInt64)
description: Lock-free atomic int cells — fetchAdd returns prior, compareAndSet, and named memory-ordering variants instead of an ordering parameter.
---

# Atomic integer cells

`AtomicInt32` and `AtomicInt64` are **twin classes**, not collaborators: one lock-free
signed-integer cell that many carriers/threads can read, write, and update without
tearing. Pick one by width — `AtomicInt32` for counters/flags, `AtomicInt64` for
sequence numbers, timestamps, and generation counters that would overflow 32 bits.
Their surfaces are identical except for the int width; everything below applies to both.

Reach here when you need a single shared integer mutated under contention. For anything
wider than one int (a queue, a map, a guarded struct) use a lock instead — see
`cajeta/concurrent/Mutex`, `RwLock`, `Semaphore`. There is **no** `AtomicReference`,
`AtomicBoolean`, or atomic float here.

## The four operations (seq_cst)

- `load() -> int` / `store(v)` — atomic read / overwrite, never tears.
- `fetchAdd(delta) -> int` — adds `delta`, returns the value held **BEFORE** the add
  (not the new value). Wraps silently on overflow. There is no `fetchSub` — pass a
  negative `delta`.
- `compareAndSet(expected, desired) -> boolean` — if the cell equals `expected`,
  replace with `desired` and return `true`; otherwise leave it and return `false`.
  The read-compare-write is one hardware step. There is no `exchange`/`getAndSet`.

`compareAndSet` returning `false` means another carrier won the race — re-read and
retry; this is the idiomatic CAS loop (see example).

## Memory ordering — named variants, no ordering parameter

The four methods above are **sequentially consistent**. To opt into weaker, faster
ordering you call a differently-named method — the ordering is baked into the method
name, there is **no `MemoryOrder` enum / ordering argument** (LLVM bakes ordering into
the IR at construction time). Only the orderings the work-stealing deque needs are
exposed today:

- `loadRelaxed` / `loadAcquire`
- `storeRelaxed` / `storeRelease`
- `fetchAddRelaxed`
- `compareAndSetAcquire` — acquire ordering (on `AtomicInt32`: success **and** failure;
  on `AtomicInt64`: success)

Use relaxed only for independent counters/statistics; pair `storeRelease` with
`loadAcquire` to publish/observe other writes across carriers. If you need an ordering
that isn't listed, it does not exist yet — add the method to the class (and its
`Cajeta.atomicI32*`/`atomicI64*` intrinsic) rather than reaching for a parameter.

## Construction, ownership, lifecycle

Construct with `heap AtomicInt32(initial)` (or `AtomicInt64(initialL)`). Each instance
**owns one heap-allocated cell** behind a private `pointer`; the destructor
(`~AtomicInt32`) frees it when the instance drops. So:

- The owner of the `AtomicInt32` handle is the owner of the cell — when the handle drops
  (scope exit, or the owning object's destructor), the cell is freed. Do not keep a raw
  reference to the cell past the handle's lifetime (use-after-free).
- To share one atomic across collaborators, store the `AtomicInt32` as a field of a
  shared object and transfer ownership with `#` at the boundary (e.g. a factory returns
  `#ConnectionLimiter` that holds the atomic). Multiple carriers then call methods on the
  same instance — the operations are the synchronization point.
- `compareAndSet`/`fetchAdd` do **not** spin, park, or wait. They return immediately;
  blocking/backoff is the caller's job (or use `Semaphore`).

## Worked example — lock-free CAS decrement (real, from ConnectionLimiter)

```cajeta
import cajeta.concurrent.AtomicInt32;

// permitCount is a shared `AtomicInt32` field seeded with `heap AtomicInt32(cap)`.
public boolean tryAdmit() {
    while (true) {
        int32 free = this.permitCount.load();
        if (free <= 0) {
            return false;                                   // none left, no permit taken
        }
        if (this.permitCount.compareAndSet(free, free - 1)) {
            return true;                                    // won the race, took a permit
        }
        // lost CAS to a concurrent admit/release — re-read and retry
    }
    return false;   // unreachable
}
```

`fetchAdd` returning the prior value makes unique-id claiming a one-liner:

```cajeta
import cajeta.concurrent.AtomicInt64;

stack seq = AtomicInt64(0L);
int64 mine = seq.fetchAdd(1L);   // mine == 0; cell now holds 1
```
