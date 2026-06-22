---
id: lang-stream-parallel
applies-to: [cajeta/lang/stream/Splittable, cajeta/lang/stream/ParallelDriver]
title: Parallel stream split protocol — Splittable + ParallelDriver
description: How Splittable.trySplit()/estimateSize() and ParallelDriver fan-out cooperate, including ownership transfer and the current single-carrier behaviour.
---

# Parallel streams: the split protocol

`Splittable<T>` is the *source* contract; `ParallelDriver` is the *orchestrator*
that consumes it. The driver peels disjoint shares off a `Splittable` source,
forks one worker per share, walks the tail itself, and combines the partials.
You normally trigger this indirectly with `xs.stream().parallel().reduce(...)`;
`ParallelDriver`'s static methods are the dispatch target underneath.

Reach for this component when you have an **array-backed / index-rangeable**
source and an **associative** terminal (reduce/fold/collect/anyMatch/allMatch/
noneMatch/findAny/forEach). It is *not* for ordered terminals, stateful
intermediates, or non-splittable sources — see "What it does NOT do".

## Members and roles

- `Splittable<T> extends Stream<T>` — marker the driver forks on. Two methods
  beyond `Stream`:
  - `#Stream<T> trySplit()` — peel half the *remaining* elements into a fresh
    owned stream; this source keeps the other half. Returns `null` when it
    can't/shouldn't split (< 2 elements left, no spatial locality).
  - `int64 estimateSize()` — best-effort remaining count; returns `-1`
    (UNKNOWN) when traversal-bound.
- `ParallelDriver` — static fork/join orchestrator. Holds the fan-out
  thresholds and `pickSplitCount`, plus the terminal entry points and the
  `async` worker functions.

Implementers of `Splittable`: `ArrayStream<T>` (halves its `[idx, limit)` index
range, no copy), `ArrayList<T>.stream()` (returns an `ArrayStream` over the
backing buffer), and `HashMap<K,V>.keys()/.values()/.entries()`.

## Collaboration / object graph and call sequence

The orchestrator runs entirely on its own fiber up to the spawn point:

1. `sz = source.estimateSize()` → `nSplits = ParallelDriver.pickSplitCount(sz)`.
2. If `nSplits < 2`, walk `source.next()` sequentially and return — no fork.
3. Otherwise loop up to `nSplits - 1` times calling `source.trySplit()`,
   stashing each returned share in a `Stream<T>[]`; stop early if it returns
   `null`. `partials[]` is sized `shareCount + 1` (one slot per share + tail).
4. `scope { ... spawn <worker>(share, ...) ... }` forks one worker per share.
   The scope's closing `}` **joins** every worker before control continues.
5. After join, the orchestrator walks the **residual** of `source` (the tail
   share it never split off) on its own fiber, writes its partial, then folds
   all partials with the terminal's combine op (and reads any short-circuit
   `AtomicInt32` flag).

`pickSplitCount(int64 sz)`: `sz < 0` (UNKNOWN) → `MAX_SPLITS`; `sz <
MIN_PER_SPLIT * 2` (i.e. < 64) → `1` (sequential); else `isqrt(sz)` clamped to
`[2, MAX_SPLITS]`. Constants: `MIN_PER_SPLIT = 32L`, `MAX_SPLITS = 8`.

## Ownership across the component boundary

- `trySplit()` returns `#Stream<T>` — **ownership transfers to the caller**
  (the driver). The driver stores it in `shares[]` and hands it to a worker.
- The split is **disjoint, not a copy**: for `ArrayStream` the backing buffer
  is shared between the source and the share; they expose non-overlapping index
  ranges. Element ownership stays with whoever owns the buffer. After a split,
  `source.next()` must yield only its half — implementers MUST honor this.
- Worker functions take the share as a borrowed `Stream<T>` parameter and drain
  it via `next()`; the join `scope` guarantees every borrow ends before the
  buffer borrow expires. Partials cross back via `#` (e.g. `reduceWorker` writes
  `partials[slot]`).
- `estimateSize()` is advisory: too-large just over-splits (harmless),
  too-small under-parallelizes. Never relied on for correctness.

Thread-safety: `estimateSize`/`trySplit` are called on the orchestrator fiber
*before* any `spawn`, so single-threaded mutation during splitting is fine.

## Current behaviour (single-carrier)

The fork topology above is fully wired (`scope`/`spawn`), but **today's
scheduler is cooperative single-carrier**, so workers run cooperatively on one
carrier — correct results, no wall-clock speedup yet. When a multi-carrier
scheduler lands, the same driver parallelizes with no code change. One method,
`countParallel`, is deliberately a plain sequential walk: it computes the
split count for future debug instrumentation but does not use it.

## What it does NOT do (avoid the dead end)

- **No ordered terminals.** `findFirst().parallel()` silently becomes
  `findAny` — encounter order is lost across shares.
- **Stateful intermediates are rejected.** A `take`/`skip` in the chain throws
  `Exception("CAJETA_ERROR_STREAM_PARALLEL_REJECT_STATEFUL ...")`; call
  `.sequential()` before the stateful op.
- **Type-changing wrappers fall back to sequential.** `MapStream<U,T>` /
  `FlatMapStream<U,T>` hide the splittable root (`unwrap()` returns `null`,
  `splittableSize()` returns `-1L`), so the chain entry points walk sequentially.
- **No correctness from the flag.** `.parallel()` never changes a result; it
  only changes scheduling. Your `fn` MUST be associative and `seed` its
  identity (collect uses a per-worker `supplier()` to avoid aliasing mutable
  accumulators); predicates side-effect-free; `forEach` `fn` commutative AND
  thread-safe.

## Worked example

```cajeta
import cajeta.lang.stream.ArrayStream;
import cajeta.lang.stream.Splittable;
import cajeta.lang.stream.ParallelDriver;

// Idiomatic path: the wrapper chain dispatches into ParallelDriver for you.
int32[] xs = heap int32[1000];
for (int32 i = 0; i < 1000; i = i + 1) { xs[i] = i + 1; }
int32 total = xs.stream().parallel().reduce(0, (a, b) -> a + b);   // 500500

// Direct path: drive a Splittable source yourself. `src` is the canonical
// splittable source; trySplit() inside the driver transfers each share by #.
Splittable<int32> src = heap ArrayStream<int32>(xs, 1000);
int32 sum = ParallelDriver.reduceParallel<int32>(src, 0, (a, b) -> a + b);

// Cross-type fold needs the 3-arg form (per-worker accumulator + combiner);
// the 2-arg fold is sequential-only.
int64 wide = xs.stream().parallel().fold<int64>(
    0L,
    (int64 acc, int32 x) -> acc + (int64) x,   // per-share accumulator
    (int64 a, int64 b) -> a + b);              // merges worker partials
```

For the source contract details see `cajeta/lang/stream/ArrayStream`; for the
underlying pull protocol see `cajeta/lang/stream/Stream`.
