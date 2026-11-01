# Stream Parallelism — Design

This is the spec for the parallel-evaluation surface on `cajeta.lang.stream.Stream<T>`.
v1 targets Java-style fork/join parallel terminals over the existing
pull protocol. The driver splits the source once, runs each split on its own
fiber, and combines the partials once. The pull contract does not change, and
existing single-threaded streams keep working unchanged.

**Companion documents:**
  - `StreamParallelism.Examples.md` is the example/error catalogue and
    landed-feature errata.
  - `StreamParallelism.ErrorHandling.md` covers exception semantics,
    the default fail-fast and the user-side tolerance patterns.

> **Implementation status.** The fork/join driver is wired and the
> chain-aware terminals (`reduce`, `fold<R>` 3-arg, `collect<R>`,
> `anyMatch`/`allMatch`/`noneMatch`, `forEach`, `findFirst`→`findAny`)
> fan out via `scope { spawn worker<...> }` over a `Splittable` root.
> The scheduler today is single-carrier and cooperative, so workers
> are fibers that interleave on one core. The structured-concurrency
> shape is in place, and it yields wall-clock parallelism unchanged once
> a multi-carrier scheduler lands. The Splittable roots are `ArrayStream`,
> `ArrayList.stream()`, and the three HashMap stream views. The older
> `countParallel` entry still walks sequentially. The driver is
> `runtime/src/cajeta/lang/stream/ParallelDriver.cajeta`.

> Reactor-style reactive, with push, async, backpressure and per-element
> scheduler hand-off, is explicitly out of scope here. It would
> parallel the pull protocol rather than reuse it, and the demand-signal
> model needs a different ownership story for in-flight elements. If
> there is a use case later, it lands as a sibling `Flux<T>` type and
> never as a retrofit on `Stream<T>`.

## Goals

1. **Composes with the existing fluent surface.** `xs.stream().parallel().
   filter(p).map(f).reduce(0, +)` works. Calling `.parallel()` on any
   stream is legal even when the underlying source cannot split, and it
   then degenerates to sequential, because the contract is "may
   parallelize."
2. **Reuses the structured-concurrency machinery from Concurrency.md.** Each
   parallel terminal is itself a `scope { ... }` body. Workers are
   `spawn`'d fibers, the terminal returns only after every worker
   completes, and exceptions propagate via the scope's existing escalation.
3. **No new ownership rules.** A worker's split is a disjoint subrange
   of the source, so one element is owned by at most one worker. Partial
   results return to the orchestrator via `#` transfer. The borrow
   checker already accepts borrows captured into scoped tasks. A worker
   reading from `int32[] xs` borrowed by the source is sound because the
   scope joins before the borrow expires.
4. **Composable with the cancellation model.** A failing worker triggers
   the scope's cancellation, and siblings observe at their next pull-loop
   `await` checkpoint and abandon early.
5. **No GIL, no extra runtime.** It lowers to the existing
   `__cajeta_task_run`, the work-stealing scheduler and the per-fiber drop chain.

## Non-goals (v1)

- **Custom executors.** v1 always uses the built-in work-stealing pool.
- **Ordered findFirst across splits.** The encounter-order variant of
  `findFirst` becomes `findAny` under parallel. An explicit
  `parallel().findFirstOrdered()` is a v2 conversation if there is
  demand. Java's `forEachOrdered` is similarly v2.
- **Stateful intermediate combinators in parallel** (`take`, `skip`).
  v1 rejects them at the terminal site with a clear error.
  `parallel().take(n).count()` either falls back to sequential or
  errors with a remediation message. v2 can add an ordered-merge step.
- **Parallel sources that don't implement `Splittable`.** There is no
  "buffered-arbitrary-stream" splitter, because fast splitting is the only
  splitting. A user wanting parallel on a non-splittable source first
  collects to an ArrayList, which is splittable, then parallels.
- **Tunable split granularity.** v1 picks `isqrt(estimateSize())`
  splits per call, capped at `MAX_SPLITS = 8` and floored to sequential
  when the source has fewer than `MIN_PER_SPLIT * 2 = 64` elements, with both
  constants on `ParallelDriver`. A manual threshold,
  `parallel(minBatchSize)`, is a v2 tuning knob.

## Surface API

### `Stream<T>.parallel(): Stream<T>`

This marks the stream as parallel-eligible by setting an `isParallel` flag
and returning `this`, with no wrapper allocation and no `#` transfer. The bit
is read by the terminal, which decides whether to fork. Calling
`.parallel()` on an already-parallel stream is a no-op, and `.sequential()`
clears the flag.

```cajeta
int32[] xs = …;
int32 total = xs.stream()
                .parallel()
                .filter((x) -> x > 0)
                .map<int32>((x) -> x * 2)
                .reduce(0, (a, b) -> a + b);
```

Internally `parallel()` returns the same stream with an `isParallel`
flag set on the head, and there is no wrapper allocation. Stateless intermediate
combinators (`filter`, `map`, `peek`, `flatMap`) propagate the flag
through their construction. Terminals branch on it.

### `Splittable<T> extends Stream<T>`

Sources that can be cheaply split implement this:

```cajeta
public interface Splittable<T> extends Stream<T> {
    // Take half of the remaining elements into a fresh stream;
    // this stream now covers the other half. Returns null when
    // the source can't / shouldn't split further (one element
    // left, no spatial locality, etc.). The returned stream
    // transfers ownership (#) — the caller is the new owner.
    public #Stream<T> trySplit();

    // Best-effort element count for split sizing. Returns -1
    // (UNKNOWN) when traversal-bound — the orchestrator falls
    // back to a fixed split fan-out.
    public int64 estimateSize();
}
```

Stdlib sources implementing `Splittable<T>` in v1:
- `ArrayStream<T>` splits by halving the index range in O(1).
- `ArrayList<T>.stream()` does the same, since it is an ArrayStream over the
  backing buffer.
- `HashMap<K,V>.keys() / values() / entries()` split by halving the
  slot-array range in O(1).

Wrapper streams (`FilterStream`, `MapStream`, `PeekStream`,
`FlatMapStream`, `TakeStream`, `SkipStream`) do not implement
`Splittable`. They wrap a source by reference and inherit splittability
through it. The orchestrator unwinds the wrapper chain at terminal
time, splits the underlying `Splittable`, then re-wraps each split
with copies of the intermediate ops. That is cheap, because `FilterStream`
and the others are 24-byte structs over a Stream+fn pair.

### Terminal dispatch

Each terminal in `Stream<T>` checks `isParallel` and forks if it can:

```cajeta
public int32 count() {
    if (this.isParallel) {
        return ParallelDriver.count(this);
    }
    int32 n = 0;
    Optional<T> o = this.next();
    while (o.isPresent()) { n = n + 1; o = this.next(); }
    return n;
}
```

`ParallelDriver` is the new internal stdlib helper that lives in
`cajeta.lang.stream.parallel`. It owns the fork-join machinery and
the per-terminal combine rules.

## Worker model

Parallel terminals lower to roughly:

```cajeta
// Inside ParallelDriver — pseudo-cajeta, illustrating the shape.
R drive(Stream<T> head, R seed, (T -> X), (R, X -> R), (R, R -> R) combiner) {
    // 1. Unwind wrapper chain to find the splittable root and the
    //    intermediate-op list. Reject stateful ops (take/skip) here
    //    with CAJETA_ERROR_STREAM_PARALLEL_REJECT_STATEFUL.
    PipelineShape shape = unwind(head);

    // 2. Decide split count. v1: ceil(sqrt(estimateSize)) capped by
    //    scheduler.availableCores(). Floor 1 (no parallelism if
    //    source is small).
    int32 splits = pickSplitCount(shape.source.estimateSize());

    // 3. Open structured-concurrency scope. Each worker writes its
    //    partial into a fresh slot transferred back at scope exit.
    R[] partials = heap R[splits];
    scope {
        Splittable<T> remaining = shape.source;
        for (int32 i = 0; i < splits - 1; i = i + 1) {
            Splittable<T> share = (Splittable<T>) remaining.trySplit();
            if (share == null) break;
            int32 slot = i;
            spawn workerBody(shape, share, slot, partials, seed, …);
        }
        // Tail piece runs on this fiber too — no point spawning to
        // run the last split when the orchestrator is otherwise idle.
        partials[splits - 1] = workerBody(shape, remaining, …);
    }   // scope joins — every spawned worker has finished here

    // 4. Combine partials. Sequential O(N) for v1; tree-reduce later.
    R acc = seed;
    for (int32 i = 0; i < splits; i = i + 1) {
        acc = combiner(acc, partials[i]);
    }
    return acc;
}
```

Each worker:
1. Owns a `Stream<T>` covering its split (transferred via `#`).
2. Re-wraps with copies of the pipeline's intermediate combinators.
   Each MapStream / FilterStream is reconstructed pointing at the
   worker's split, which is cheap because no source data is copied.
3. Walks `next()` to exhaustion on its own fiber.
4. Hands its partial back to the orchestrator's slot.

The orchestrator never touches an element directly until the workers
have all transferred their partials at scope exit. The borrow
checker is satisfied because the source data, such as the `int32[] xs`
buffer, is borrowed by `ArrayStream<int32>`, which is owned by the
orchestrator's stream chain. The workers' splits are sub-streams
over the same backing buffer, valid for the scope's lifetime, and the
scope joins before the orchestrator can drop them.

## Per-terminal rules

| Terminal | Parallel? | Combine | Notes |
| -------- | --------- | ------- | ----- |
| `count` | yes | sum | trivial. |
| `forEach((T)→void)` | yes | none | lambda must be side-effect-clean across threads. Order is unspecified under parallel — same as Java. |
| `anyMatch / allMatch / noneMatch` | yes | OR / AND / AND | short-circuiting; first worker to find result triggers scope cancellation. |
| `findFirst` | yes | "any non-empty" | semantics shift to `findAny` under parallel — first encountered match by any worker wins. Document the change at the call site (compile-time lint: warn that ordering isn't preserved). v2 may add `findFirstOrdered`. |
| `reduce(T seed, (T,T)→T fn)` | yes | `fn` | requires `fn` associative AND `seed` to be its identity. Programmer responsibility (Java's same contract). |
| `fold<R>(R seed, (R,T)→R fn)` | **needs combiner** | n/a | v1 sequential-only when called on parallel stream; rejects with a remediation message pointing at the new `fold<R>(seed, fn, combiner)` overload below. |
| `fold<R>(R seed, (R,T)→R fn, (R,R)→R combiner)` | yes | `combiner` | new overload; `combiner` merges partials. |
| `collect<R>(Collector<T, R>)` | yes | `combiner` | Each worker calls the collector's `supplier()` for its own partial `R`, accumulates, and the orchestrator merges partials with `combiner`. Both fields are required on `Collector`, so every collector is parallel-ready. |
| `take(n) / skip(n)` (intermediate, not terminal) | **rejected** | n/a | stateful; would need ordered merge. The parallel driver's chain-walk throws `CAJETA_ERROR_STREAM_PARALLEL_REJECT_STATEFUL` (runtime, not compile-time — the parallel flag is a runtime property) with a remediation suggesting `.sequential().take(n)`. |

The shipped `Collector<T, R>` (`runtime/src/cajeta/collection/Collector.cajeta`):

```cajeta
public class Collector<T, R> {
    public () -> #R supplier;      // fresh accumulator instance, per worker
    public (R, T) -> #R accumulator;
    public (R, R) -> #R combiner;  // merge two partials

    public Collector(() -> #R supplier,
                     (R, T) -> #R accumulator,
                     (R, R) -> #R combiner);   // all three required
}
```

A per-worker `supplier()` in place of a single shared `seed` is what
makes a mutable `R` like `ArrayList` safe under parallel. Each worker
accumulates into its own instance with no aliasing. `Collectors.toList<T>()`
supplies `() -> heap ArrayList<T>()` with an `appendAll`-based combiner.

## Ownership and the borrow checker

A parallel terminal opens a `scope { }` block. Each worker is a
`spawn` inside that scope, so:

- **Captures from the orchestrator's frame** (the source array, the
  intermediate-op closures, the seed value) are valid borrows, because
  the scope joins before they expire.
- **The accumulator `R`** is captured by value for a primitive or by borrow
  for a class. For a class-typed accumulator used in a collector, each
  worker needs its own instance. The driver resolves this by calling
  `Collector.supplier: () -> #R` per worker instead of sharing one
  instance. `Collectors.toList` passes `() -> heap ArrayList<T>()`,
  one fresh list per worker.
- **Partials returning to the orchestrator** travel via `#` transfer.
  The combiner takes ownership, and the worker's partial is dropped from
  the worker's drop chain at hand-off.

The per-fiber drop chain promotion that landed in R5 already gives
each worker its own chain, so a worker's `__cajeta_drop_push` does not
touch the orchestrator's. The live-set is process-global with atomic
claim semantics, which is already correct under workers running in parallel.

## Lint and diagnostics

Two new compile-time signals:

1. **`[parallel-stateful-op]`** fires on using `take`/`skip` after `parallel()`
   in the same chain. The suggestion is `xs.stream().parallel().filter(…)
   .sequential().take(n)`.
2. **`[parallel-collector-no-combiner]`** fires on `collect(c)` on a parallel
   stream where `c.combiner == null`. The suggestion is to add a combiner to
   the collector or drop the `.parallel()` call.

The runtime exceptions, raised only if the static check is bypassed by
dynamic shape construction, are:

- `CAJETA_ERROR_STREAM_PARALLEL_REJECT_STATEFUL`
- `CAJETA_ERROR_STREAM_PARALLEL_NO_COMBINER`

## Implementation phases

**P1 — Splittable<T> + parallel() bit + driver scaffolding.**
- `cajeta.lang.stream.Splittable<T>` interface.
- `ArrayStream<T>.trySplit()` / `estimateSize()`.
- `Stream<T>.parallel()` / `sequential()` flag-flip methods (instance
  field `isParallel`; copy into wrapper combinators on construction).
- `ParallelDriver` class in `cajeta.lang.stream.parallel` with the
  unwind + split + scope + combine skeleton. v1 supports only
  `count` to keep the surface narrow while the plumbing settles.
- Tests: trivial count parallels, count on non-splittable falls back
  to sequential, parallel through filter/map composes.

**P2 — Reduce-shaped terminals.**
- `reduce(T, (T,T)→T)` with associativity-required note.
- `anyMatch / allMatch / noneMatch` with scope cancellation.
- `forEach` with explicit "order unspecified" note.
- Tests: each terminal with both splittable and non-splittable sources.

**P3 — Cross-type combinators.**
- `fold<R>(seed, fn, combiner)` heap overload (sequential-friendly
  three-arg form stays single-threaded).
- `Collector<T, R>` combiner field + `Collectors.toList` combiner.
- `collect` parallel path.
- Tests: parallel sum-into-int64 via fold<int64>, parallel toList,
  parallel `Collectors` chain.

**P4 — Source coverage + diagnostics.**
- `HashMap` stream views become Splittable. **Shipped.**
  `HashMapKeyStream` / `HashMapValueStream` / `HashMapEntryStream`
  each `implements Splittable<T>` and override
  `trySplit / estimateSize / splittableSize / trySplitRoot`,
  halving the slot-array index range with the same disjoint-share model as
  `ArrayStream`. This is verified end-to-end across count / reduce / fold-
  with-combiner / anyMatch / allMatch / noneMatch / forEach
  terminals in `test/collections/HashMapStreamParallelTests.cpp`.
- Lint passes for `[parallel-stateful-op]` /
  `[parallel-collector-no-combiner]`.
- Runtime exceptions wired.
- Tests: HashMap parallel terminals; lint-as-error suites.

**P5 — Tuning + cancellation.**
- `pickSplitCount` reads scheduler core count.
- `findAny` semantics for `findFirst` under parallel; lint warns
  about ordering loss.
- Scope-cancellation hookup for short-circuiting terminals. Workers
  poll a flag inside their pull loop, the first match flips it, and the
  others bail at their next checkpoint.

## Out-of-scope edge cases (documented decisions)

- **Nested parallelism.** A parallel stream that produces a parallel
  stream via flatMap. v1 sequentializes the inner, and only the outer
  parallelizes. The scheduler's work-stealing makes nested parallel
  flatten naturally if we ever lift this, and the restriction is for the
  v1 driver's bookkeeping simplicity.
- **Reduction with non-associative ops.** This is the programmer's
  responsibility, the same as Java's contract. There is no runtime check.
- **`Stream<T>` where `T` is a non-`Send` type.** Cajeta's existing
  `#` transfer rule already governs cross-thread handoff, and the
  parallel driver inherits whatever the language enforces. A worker
  pulling a class-typed `T` from its split owns it for the worker's
  fiber lifetime. Transferring to the orchestrator only happens if
  the terminal accumulates `T` into the partial, as in reduce / collect of
  class-typed Ts, in which case the partial transfer must use `#`.
- **Source mutation during traversal.** The contract is the same as
  sequential, which is undefined behavior. The HashMap stream views already
  specify snapshot semantics, and that property carries over.

## Locked decisions (2026-05-22)

- **findFirst** silently becomes findAny under parallel, and the lint
  `[parallel-findfirst-unordered]` warns about ordering loss. The fluent
  surface stays intact.
- **OOM at spawn** always raises `SystemResourceException`, with no
  silent fallback. This surfaces brittle workloads early.
- **`Collector<T, R>` combiner is REQUIRED.** The shipped ctor is
  3-arg `(supplier, accumulator, combiner)`. Note that `supplier: () -> #R`
  replaced the originally-planned `seed` field so each parallel worker
  gets its own accumulator instance (see Examples.md § 7.9 errata).
  Existing 2-arg call sites were swept in the P1 prep commit, at the
  Collectors.toList and CollectorTests sites.
- **Worker stack-trace frames** named `<parallel worker N>` using
  the split index.

## Open design questions

These are out of scope for v1 and listed for future tracking.

- **Tree-reduce of partials** instead of orchestrator-linear sweep.
  v1's linear sweep is fine for the split counts the driver picks,
  4–32. Defer it to v2 when measurements demand it.
- **Custom executor surface.** A `parallel(Executor e)` overload.
  It is out of scope for v1, and the Executor type itself is not designed yet.
