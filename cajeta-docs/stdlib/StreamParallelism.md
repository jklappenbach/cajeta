# Stream Parallelism — Design

Spec for the parallel-evaluation surface on `cajeta.lang.stream.Stream<T>`.
v1 targets Java-style **fork/join parallel terminals over the existing
pull protocol** — split the source once, run each split on its own fiber,
combine partials once. The pull contract doesn't change; existing single-
threaded streams keep working unchanged.

> Reactor-style reactive (push + async + backpressure + per-element
> scheduler hand-off) is explicitly out of scope here — that would
> parallel the pull protocol rather than reuse it, and the demand-signal
> model needs a different ownership story for in-flight elements. If
> there's a use case later, it lands as a sibling `Flux<T>` type, not
> a retrofit on `Stream<T>`.

## Goals

1. **Composes with the existing fluent surface.** `xs.stream().parallel().
   filter(p).map(f).reduce(0, +)` works; calling `.parallel()` on any
   stream is legal even when the underlying source can't split (it
   degenerates to sequential — the contract is "may parallelize," not
   "must").
2. **Reuses the structured-concurrency machinery from Thread.md.** Each
   parallel terminal is itself a `scope { ... }` body — workers are
   `spawn`'d fibers, the terminal returns only after every worker
   completes, exceptions propagate via the scope's existing escalation.
3. **No new ownership rules.** A worker's split is a disjoint subrange
   of the source; one element is owned by at most one worker. Partial
   results return to the orchestrator via `#` transfer. The borrow
   checker already accepts borrows captured into scoped tasks (a worker
   reading from `int32[] xs` borrowed by the source is sound because the
   scope joins before the borrow expires).
4. **Composable with the cancellation model.** A failing worker triggers
   the scope's cancellation; siblings observe at their next pull-loop
   `await` checkpoint and abandon early.
5. **No GIL, no extra runtime.** Lowers to the existing
   `__cajeta_task_run` + work-stealing scheduler + per-fiber drop chain.

## Non-goals (v1)

- **Custom executors.** v1 always uses the built-in work-stealing pool.
- **Ordered findFirst across splits.** The encounter-order variant of
  `findFirst` becomes `findAny` under parallel; explicit
  `parallel().findFirstOrdered()` is a v2 conversation if there's
  demand. Java's `forEachOrdered` is similarly v2.
- **Stateful intermediate combinators in parallel** (`take`, `skip`).
  v1 rejects them at the terminal site with a clear error;
  `parallel().take(n).count()` either falls back to sequential or
  errors with a remediation message. v2 can add an ordered-merge step.
- **Parallel sources that don't implement `Splittable`.** No
  "buffered-arbitrary-stream" splitter — fast splitting is the only
  splitting. A user wanting parallel on a non-splittable source first
  collects to an ArrayList (which is splittable), then parallels.
- **Tunable split granularity.** v1 picks `ceil(sqrt(estimateSize()))`
  splits per call, capped by the scheduler's core count. Manual
  threshold (`parallel(minBatchSize)`) is a v2 tuning knob.

## Surface API

### `Stream<T>.parallel(): #Stream<T>`

Marks the stream as parallel-eligible. Returns the same stream
shape (still pull-protocol) — the bit is read by the terminal, which
decides whether to fork. Calling `.parallel()` on an already-parallel
stream is a no-op; calling `.sequential()` flips it back.

```cajeta
int32[] xs = …;
int32 total = xs.stream()
                .parallel()
                .filter((x) -> x > 0)
                .map<int32>((x) -> x * 2)
                .reduce(0, (a, b) -> a + b);
```

Internally `parallel()` returns the SAME stream with an `isParallel`
flag set on the head — no wrapper allocation. Stateless intermediate
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
- `ArrayStream<T>` — splits by halving the index range. O(1).
- `ArrayList<T>.stream()` — same, since it's an ArrayStream over the
  backing buffer.
- `HashMap<K,V>.keys() / values() / entries()` — splits by halving the
  slot-array range. O(1).

Wrapper streams (`FilterStream`, `MapStream`, `PeekStream`,
`FlatMapStream`, `TakeStream`, `SkipStream`) do **not** implement
`Splittable`. They wrap a source by reference and inherit splittability
through it — the orchestrator unwinds the wrapper chain at terminal
time, splits the underlying `Splittable`, then re-wraps each split
with copies of the intermediate ops (cheap — `FilterStream` etc. are
24-byte structs over a Stream+fn pair).

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
    R[] partials = new R[splits];
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
2. Re-wraps with copies of the pipeline's intermediate combinators
   (each MapStream / FilterStream is reconstructed pointing at the
   worker's split — cheap, no source data copied).
3. Walks `next()` to exhaustion on its own fiber.
4. Hands its partial back to the orchestrator's slot.

The orchestrator never touches an element directly until the workers
have all transferred their partials at scope exit. The borrow
checker is satisfied because the source data (e.g. the `int32[] xs`
buffer) is borrowed by `ArrayStream<int32>` which is owned by the
orchestrator's stream chain — the workers' splits are sub-streams
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
| `collect<R>(Collector<T, R>)` | yes if collector has a combiner | `combiner` | `Collector<T, R>` gains a `(R, R) -> R combiner` field (currently has `seed` + `accumulator`). Collectors that can't combine (e.g. one accumulating into a non-mergeable structure) leave `combiner = null`, and `collect` under parallel rejects with the same shape error as no-combiner fold. |
| `take(n) / skip(n)` (intermediate, not terminal) | **rejected** | n/a | stateful; would need ordered merge. Compile-time error from the parallel driver's `unwind`: `CAJETA_ERROR_STREAM_PARALLEL_REJECT_STATEFUL` with a remediation suggesting `.sequential().take(n)`. |

The `Collector<T, R>` extension:

```cajeta
public class Collector<T, R> {
    public final R seed;
    public final (R, T) -> R accumulator;
    public final (R, R) -> R combiner;   // NEW. null = sequential-only.
}
```

Backwards-compat: existing two-arg `Collector` ctor calls continue to
compile and produce a sequential-only collector. Stdlib's
`Collectors.toList<T>()` gains a combiner that appends one ArrayList
into another.

## Ownership and the borrow checker

A parallel terminal opens a `scope { }` block. Each worker is a
`spawn` inside that scope, so:

- **Captures from the orchestrator's frame** (the source array, the
  intermediate-op closures, the seed value) are valid borrows — the
  scope joins before they expire.
- **The seed `R`** is captured by-value (primitive) or by-borrow
  (class). For a class-typed seed used in a collector, each worker
  needs ITS OWN copy of the seed to accumulate into. The driver
  resolves this by calling `Collector.seedFactory: () -> R` per
  worker rather than capturing the seed directly. `Collectors.toList`
  passes `() -> heap ArrayList<T>()` — one fresh list per worker.
- **Partials returning to the orchestrator** travel via `#` transfer.
  The combiner takes ownership; the worker's partial is dropped from
  the worker's drop chain at hand-off.

The per-fiber drop chain promotion that landed in R5 already gives
each worker its own chain, so a worker's `__cajeta_drop_push` doesn't
touch the orchestrator's. The live-set is process-global with atomic
claim semantics — already correct under workers running in parallel.

## Lint and diagnostics

Two new compile-time signals:

1. **`[parallel-stateful-op]`** — using `take`/`skip` AFTER `parallel()`
   in the same chain. Suggestion: `xs.stream().parallel().filter(…)
   .sequential().take(n)`.
2. **`[parallel-collector-no-combiner]`** — `collect(c)` on a parallel
   stream where `c.combiner == null`. Suggestion: add a combiner to
   the collector OR drop the `.parallel()` call.

Runtime exception (only if the static check is bypassed by dynamic
shape construction):

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
- `fold<R>(seed, fn, combiner)` new overload (sequential-friendly
  three-arg form stays single-threaded).
- `Collector<T, R>` combiner field + `Collectors.toList` combiner.
- `collect` parallel path.
- Tests: parallel sum-into-int64 via fold<int64>, parallel toList,
  parallel `Collectors` chain.

**P4 — Source coverage + diagnostics.**
- `HashMap` stream views become Splittable.
- Lint passes for `[parallel-stateful-op]` /
  `[parallel-collector-no-combiner]`.
- Runtime exceptions wired.
- Tests: HashMap parallel terminals; lint-as-error suites.

**P5 — Tuning + cancellation.**
- `pickSplitCount` reads scheduler core count.
- `findAny` semantics for `findFirst` under parallel; lint warns
  about ordering loss.
- Scope-cancellation hookup for short-circuiting terminals (workers
  poll a flag inside their pull loop; first match flips it and the
  others bail at their next checkpoint).

## Out-of-scope edge cases (documented decisions)

- **Nested parallelism.** A parallel stream that produces a parallel
  stream via flatMap. v1 sequentializes the inner — only the outer
  parallelizes. The scheduler's work-stealing makes nested parallel
  flatten naturally if we ever lift this; the restriction is for the
  v1 driver's bookkeeping simplicity.
- **Reduction with non-associative ops.** Programmer responsibility,
  same as Java's contract. No runtime check.
- **`Stream<T>` where `T` is a non-`Send` type.** Cajeta's existing
  `#` transfer rule already governs cross-thread handoff; the
  parallel driver inherits whatever the language enforces. A worker
  pulling a class-typed `T` from its split owns it for the worker's
  fiber lifetime; transferring to the orchestrator only happens if
  the terminal accumulates `T` into the partial (reduce / collect of
  class-typed Ts), in which case the partial transfer must use `#`.
- **Source mutation during traversal.** Same contract as sequential:
  undefined behavior. The HashMap stream views already specify
  snapshot semantics; that property carries over.

## Locked decisions (2026-05-22)

- **findFirst** silently becomes findAny under parallel; lint
  `[parallel-findfirst-unordered]` warns about ordering loss. Fluent
  surface stays intact.
- **OOM at spawn** always raises `SystemResourceException` — no
  silent fallback. Surfaces brittle workloads early.
- **`Collector<T, R>` combiner is REQUIRED.** 3-arg ctor `(seed,
  accumulator, combiner)`. Existing 2-arg call sites are swept in
  the P1 prep commit (Collectors.toList + 4 CollectorTests sites).
- **Worker stack-trace frames** named `<parallel worker N>` using
  the split index.

## Open design questions

(Out of scope for v1 — listed for future tracking.)

- **Tree-reduce of partials** instead of orchestrator-linear sweep.
  v1's linear sweep is fine for the split counts the driver picks
  (4–32). Defer to v2 when measurements demand it.
- **Custom executor surface.** `parallel(Executor e)` overload.
  Out of scope for v1; the Executor type itself isn't designed yet.
