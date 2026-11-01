# Stream Parallelism — Examples & Error Reference

This document is the companion to `StreamParallelism.md`. It is a
pre-implementation walk-through of every user-visible shape, covering
the happy path, compile-time rejections, runtime exceptions, and
system-error edge cases. Each entry is what the test suite should pin
and what the documentation should show. Review this BEFORE any code
lands so the diagnostic surface is locked in.

The error-message blocks below are aspirational. They are what the
compiler and runtime SHOULD print. Anywhere the format is ambiguous,
treat the example as the source of truth.

---

## §1 Happy path

### 1.1 — Parallel count

```cajeta
int32[] xs = heap int32[10_000];
for (int32 i = 0; i < 10_000; i = i + 1) { xs[i] = i; }

int32 n = xs.stream().parallel().count();
// n == 10_000
```

What happens:
- An `ArrayStream<int32>` is constructed over `xs`, with
  `isParallel = true`.
- The terminal `count()` checks the flag and dispatches to
  `ParallelDriver.count(this)`.
- The driver calls `estimateSize() = 10_000` and picks `splits =
  isqrt(10_000) = 100`, capped at `MAX_SPLITS = 8`.
- N workers are `spawn`'d. Each pulls its split to exhaustion and
  returns its local count via `#int32` transfer.
- The orchestrator sums the partials into `n`.

### 1.2 — Parallel reduce (sum)

```cajeta
int64 total = xs.stream()
                .parallel()
                .reduce(0L, (a, b) -> (int64) (a + b));
// total == sum of xs
```

The contract is that the reduce op must be associative AND the seed
(`0L` here) must be its identity element. That is the programmer's
responsibility, the same as Java's `Stream.reduce(BinaryOperator)`.

### 1.3 — Parallel filter + map + reduce

```cajeta
int32 sumOfDoubledPositives = xs.stream()
    .parallel()
    .filter((x) -> x > 0)
    .map<int32>((x) -> x * 2)
    .reduce(0, (a, b) -> a + b);
```

The pipeline shape is `ArrayStream → FilterStream → MapStream → reduce`.
The driver unwinds, splits `ArrayStream` only, and re-wraps each split
with a fresh FilterStream + MapStream pointing at the worker's slice.
Each worker runs its full pipeline locally, the partials are int32,
and they combine via `+`.

### 1.4 — Short-circuiting `anyMatch`

```cajeta
boolean hasNeg = xs.stream()
                   .parallel()
                   .anyMatch((x) -> x < 0);
```

If any worker finds a match, it sets the scope's cancellation flag.
Siblings observe at their next pull-loop checkpoint and bail. The
scope joins, and the orchestrator returns `true`. If no worker finds a
match, all run to exhaustion and the orchestrator returns `false`.

### 1.5 — Parallel fold<R> with combiner

```cajeta
int64 sum = xs.stream()
    .parallel()
    .fold<int64>(
        0L,                                   // seed (each worker gets fresh)
        (acc, x) -> acc + (int64) x,         // per-element accumulator
        (left, right) -> left + right);      // partial combiner
```

The three-arg `fold<R>` is the parallel-friendly form. The 2-arg form
`fold<R>(seed, fn)` is sequential-only, and calling it on a parallel
stream is rejected at compile time (see §2.2).

### 1.6 — Parallel collect with combiner-bearing Collector

```cajeta
ArrayList<int32> doubled #= xs.stream()
    .parallel()
    .filter((int32 x) -> { return x > 0; })
    .map<int32>((int32 x) -> { return x * 2; })
    .collect<ArrayList<int32>>(Collectors.toList<int32>());
```

`collect` returns an owned `#R`, so the whole pipeline fits in one
statement. The streams that `stream()`, `filter` and `map` build are
freed at the end of the statement. The collected list is a fresh value
that outlives them. Bind it with `#=`. A stage kept past its statement
is different. `Stream<int32> kept #= xs.stream().filter(p);` compiles,
but the stage borrows the stream temporary and reads freed memory once
the statement ends. Bind the source first when you keep a stage.

`Collectors.toList<T>` ships with a combiner (`(a, b) -> a.appendAll(b)`).
Each worker accumulates into its own ArrayList, and the orchestrator
appends them in encounter order from the split fan-out. The result has
all elements, and the order across splits is the source's natural
order, since splits are disjoint index ranges.

### 1.7 — Parallel forEach (side-effecting, order unspecified)

```cajeta
AtomicInt32 counter = heap AtomicInt32(0);
xs.stream()
  .parallel()
  .forEach((x) -> { if (x > 0) counter.incrementAndGet(); });

int32 positives = counter.get();
```

Order across workers is not specified. The lambda's side effects
must therefore be commutative AND thread-safe. This example uses
`AtomicInt32` for that reason. Non-thread-safe captures, such as a
plain `int32` accumulator, are rejected by §2.5.

### 1.8 — `.parallel().sequential()` flips back

```cajeta
xs.stream()
  .parallel()
  .filter((x) -> x > 0)
  .sequential()     // drop back to sequential for the stateful tail
  .take(5)
  .count();
```

`.sequential()` returns the same stream with `isParallel = false`.
It is the escape hatch for stateful intermediate ops (§2.1).

### 1.9 — Idempotent `.parallel()`

```cajeta
xs.stream().parallel().parallel().count();   // same as one .parallel()
```

Setting the flag twice is a no-op. There is no allocation and no
warning.

### 1.10 — Parallel on HashMap stream view

```cajeta
HashMap<String, int32> scores = ...;
int64 total #= scores.values()
                    .parallel()
                    .map<int64>((v) -> (int64) v)
                    .reduce(0L, (a, b) -> a + b);
```

`HashMap.values()` returns a `HashMapValueStream<K,V>` which implements
`Splittable<V>` by halving the slot-array index range. The stream has
snapshot semantics, since it captures slot-array refs at construction.
A concurrent put or resize during the parallel traversal is undefined
behavior, the same contract as the sequential traversal.

### 1.11 — Fallback to sequential on non-splittable source

```cajeta
Stream<int32> generator = heap CustomCounter(100);   // not Splittable
int32 n = generator.parallel().count();
// n == 100; runs sequentially — no error
```

The driver's `unwind` step finds the source is not `Splittable<T>`,
records a `[parallel-no-split]` diagnostic at debug-mode build time,
which is off in release, and runs the pipeline on the orchestrator
thread. There is no compile error, because `parallel()` is a hint.

### 1.12 — Source too small to parallelize

```cajeta
int32[] tiny = {1, 2, 3};
int32 n = tiny.stream().parallel().count();
// n == 3; runs sequentially
```

`estimateSize() = 3` falls below the driver's split-count threshold.
`pickSplitCount` returns 1 when the source has fewer than
`MIN_PER_SPLIT * 2 = 64` elements, so anything under 64 stays
sequential. There is no error.

---

## §2 Compile-time rejections

Each rejection points at the offending source position, explains why,
and offers a concrete remediation.

### 2.1 — Stateful intermediate op after parallel()

```cajeta
xs.stream().parallel().take(5).count();
```

```
error[parallel-stateful-op]: stream operation 'take' requires ordered
       traversal, which the parallel driver doesn't preserve
  --> example.cajeta:5:28
   5 |     xs.stream().parallel().take(5).count();
     |                            ^^^^^^^ rejected here

  = note: 'take' and 'skip' depend on global encounter order across
          splits. The parallel driver splits the source by index range
          but workers run independently, so "take the first 5" has no
          well-defined meaning when worker 2 might finish before worker 1.

  = help: drop back to sequential before the stateful op:
            xs.stream().parallel().filter(p).sequential().take(5).count()

          or remove .parallel() if the stateful op dominates the cost.
```

The same shape applies to `skip(n)`.

### 2.2 — `fold<R>` two-arg overload on parallel stream

```cajeta
xs.stream().parallel().fold<int64>(0L, (acc, x) -> acc + (int64) x);
```

```
error[parallel-fold-no-combiner]: fold<R> on a parallel stream
       requires a partial-combiner function
  --> example.cajeta:5:28
   5 |     xs.stream().parallel().fold<int64>(0L, (acc, x) -> acc + x);
     |                            ^^^^^^^^^^^ rejected here

  = note: each worker computes its own R partial. Without a combiner
          the orchestrator can't merge them — the per-element
          accumulator's type (R, T) -> R isn't a valid combine
          shape (no way to reduce R + R).

  = help: use the three-arg fold:
            xs.stream().parallel().fold<int64>(
                0L,
                (acc, x) -> acc + (int64) x,
                (a, b) -> a + b);            // combiner

          or sequentialize:
            xs.stream().parallel().filter(p).sequential().fold(...);
```

### 2.3 — `collect<R>` with a no-combiner Collector

```cajeta
public class MyCollector implements Collector<int32, MyAggregate> {
    public MyAggregate seed() { return heap MyAggregate(); }
    public MyAggregate accumulate(MyAggregate a, int32 x) { ... }
    // no combine() — only sequential collect would work
}

xs.stream().parallel().collect<MyAggregate>(heap MyCollector());
```

```
error[parallel-collector-no-combiner]: collector 'MyCollector' has no
       combine method; parallel collect cannot merge worker partials
  --> example.cajeta:14:32
  14 |     xs.stream().parallel().collect<MyAggregate>(heap MyCollector());
     |                            ^^^^^^^ rejected here

  = note: collector at line 5 defines seed() + accumulate() but no
          combine(MyAggregate, MyAggregate) -> MyAggregate.

  = help: implement the combine method:
            public MyAggregate combine(MyAggregate left, MyAggregate right) {
                // merge right into left (or build fresh) and return.
            }

          or sequentialize:
            xs.stream().filter(p).sequential().collect<MyAggregate>(...);
```

### 2.4 — `reduce` on parallel stream with non-identity seed (lint, not error)

```cajeta
int32 result = xs.stream().parallel().reduce(100, (a, b) -> a + b);
// 100 added once per worker = N partials × 100, then summed
// → result == sum(xs) + N*100, not sum(xs) + 100.
```

```
warning[parallel-reduce-nonzero-seed]: seed '100' may not be the
       identity element for the reduction operator
  --> example.cajeta:5:43
   5 |     int32 result = xs.stream().parallel().reduce(100, (a, b) -> a + b);
     |                                           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^

  = note: the parallel driver applies the seed once per worker partial
          and once more at the orchestrator combine — using a non-
          identity seed produces a result that depends on the split
          count, which is non-deterministic across machines / runs.

  = help: pass 0 (the identity for '+') and add the bias outside the
          stream:
            int32 result = 100 + xs.stream().parallel().reduce(0, (a,b) -> a+b);

          or sequentialize if the bias is load-bearing.

  = suppress: @SuppressLint("parallel-reduce-nonzero-seed") on the
              enclosing method if the math is verified intentional.
```

This is a lint. It cannot be a hard error, because the compiler cannot
prove whether a seed is an identity, since it cannot see into
user-defined ops. The lint fires for any non-zero, non-empty-string,
non-empty-collection seed in common cases, and `@SuppressLint` opts
out per call site.

### 2.5 — Capture-mutation in a parallel lambda (lint)

```cajeta
int32 total = 0;
xs.stream().parallel().forEach((x) -> { total = total + x; });
```

```
warning[parallel-shared-mutation]: lambda mutates captured local
       'total' from multiple workers — data race
  --> example.cajeta:6:46
   6 |     xs.stream().parallel().forEach((x) -> { total = total + x; });
     |                                              ^^^^^ mutated here

  = note: the parallel driver runs the lambda on N fibers concurrently.
          Plain reads/writes of a captured class-or-primitive local
          aren't atomic; the final value depends on interleaving.

  = help: aggregate via reduce, which gives each worker its own seed
          copy and combines without sharing:
            int32 total = xs.stream().parallel().reduce(0, (a, b) -> a + b);

          or use an atomic when forEach is genuinely the right shape:
            AtomicInt32 total = heap AtomicInt32(0);
            xs.stream().parallel().forEach((x) -> total.addAndGet(x));
            int32 result = total.get();

  = suppress: @SuppressLint("parallel-shared-mutation") if the capture
              is read-only despite the syntactic write (e.g. mutates an
              already-atomic / already-locked structure via method).
```

For detection, the lint walks the lambda body looking for an
assignment LHS that resolves to a captured local, which is an
`IdentifierExpression` resolving outside the lambda's own scope.
Captures of atomic types (`AtomicInt32`, `AtomicInt64`, `AtomicBool`,
`AtomicRef<T>`) are exempt. Read-only captures are fine.

### 2.6 — `parallel()` on a stream of non-Send type

```cajeta
public class HandleNotSend {
    private SystemHandle h;
    // marked @NotSend — see Concurrency.md § Sendability
}

xs.stream().parallel().map<HandleNotSend>((x) -> heap HandleNotSend(x))
                      .forEach((h) -> use(h));
```

```
error[parallel-not-send]: type 'HandleNotSend' carries the @NotSend
       marker; values cannot cross thread boundaries
  --> example.cajeta:8:24
   8 |     xs.stream().parallel().map<HandleNotSend>((x) -> ...)
     |                            ^^^^^^^^^^^^^^^^^^^^ produces non-Send T

  = note: the parallel driver transfers partials between workers and
          the orchestrator via '#'. Types declared @NotSend reject
          this transfer at compile time — see docs/specification/
          Concurrency.md § Sendability.

  = help: build the HandleNotSend values on the orchestrator, after
          the parallel terminal:
            int32[] indices = xs.stream().parallel().filter(p)
                                .toArray<int32>();
            for (int32 i : indices) { use(heap HandleNotSend(i)); }
```

The @NotSend marker does not exist yet, and it will be added if a use
case surfaces. It is listed here so the diagnostic shape is committed
if we add it.

### 2.7 — Nested `parallel()` calls (warn, not error)

```cajeta
xs.stream().parallel().flatMap<int32>((int32 x) -> {
    int32[] inner = [x, x + 1];
    Stream<int32> s #= inner.stream();
    s.parallel();                        // inner parallel
    return #s;
}).count();
```

```
warning[parallel-nested]: inner stream marked .parallel() inside a
       parallel outer flatMap will run sequentially
  --> example.cajeta:4:7
   4 |     s.parallel();
     |       ^^^^^^^^^^

  = note: v1 doesn't propagate parallelism through flatMap inner
          streams — the outer parallel already saturates the worker
          pool; nested parallel would oversubscribe.

  = help: drop the inner .parallel() (it's a no-op anyway), or move
          parallelism to the inner pipeline if the outer is short:
            int32 N = xs.stream().count();
            if (N < 16) { /* drop outer .parallel() */ }
```

---

## §3 Runtime exceptions

### 3.1 — Worker throws; siblings cancel

```cajeta
xs.stream().parallel().forEach((x) -> {
    if (x < 0) throw heap IllegalArgumentException("negative: " + x);
    process(x);
});
```

Behavior:
1. Worker N hits negative, throws `IllegalArgumentException`.
2. Exception captured into the worker's Task exception slot
   (existing R5-D-lite machinery).
3. Worker N's scope frame triggers cancellation on siblings.
4. Sibling workers observe at their next pull-loop checkpoint, since
   the driver injects an explicit `__cajeta_check_cancelled()` call
   after each `next()` pull. They bail their loops and return early.
5. The scope joins. `scope_exit` finds the first exception and
   re-raises it on the orchestrator. Other workers' exceptions, of
   which there are none in this scenario, land in `cause.suppressed`.

The stack trace, auto-printed when uncaught:

```
cajeta.error.IllegalArgumentException: negative: -3
  at example.<lambda#1>(example.cajeta:6)
  at cajeta.lang.stream.parallel.ParallelDriver.workerBody(ParallelDriver.cajeta:148)
  at cajeta.lang.stream.parallel.<parallel worker 2>(ParallelDriver.cajeta:96)
  at example.run(example.cajeta:5)
```

The `<parallel worker 2>` synthetic frame names the split index so
users can correlate with input data layout.

### 3.2 — Two workers throw simultaneously

```cajeta
xs.stream().parallel().forEach((x) -> {
    if (x == 0) throw heap ArithmeticException("zero");
    if (x < 0)  throw heap IllegalArgumentException("negative");
    process(100 / x);
});
```

If worker 1 throws ArithmeticException and worker 3 throws
IllegalArgumentException before either observes the other's
cancellation:

- Both exceptions are captured in their respective Task slots.
- The scope joins. `scope_exit` raises the FIRST-completed worker's
  exception as the primary. The other's exception is attached as
  `primary.suppressed`.

```
cajeta.error.ArithmeticException: zero
  at example.<lambda#1>(example.cajeta:5)
  at cajeta.lang.stream.parallel.<parallel worker 1>(...)
  at example.run(example.cajeta:4)
  Suppressed:
    cajeta.error.IllegalArgumentException: negative
      at example.<lambda#1>(example.cajeta:6)
      at cajeta.lang.stream.parallel.<parallel worker 3>(...)
      at example.run(example.cajeta:4)
```

It is "first-completed" rather than "first-thrown" because we drain
the scope's child list in registration order at exit time and pick the
first non-null exception slot. It is documented as such, and users
wanting exception priority semantics get `.sequential()`.

> **Note — supersedes the `suppressed` description above.** The locked
> error-semantics decision (`StreamParallelism.ErrorHandling.md` § 2.2,
> § 5) is that the first throw wins and other workers' exceptions are
> LOST. There is no `AggregateException` and no `suppressed` attachment
> at the stream layer. The `Suppressed:` trace shown here is
> aspirational. Treat ErrorHandling.md as authoritative. A per-task
> outcome list, if it ever lands, lives at the `scope` layer rather
> than the stream terminal.

### 3.3 — Combiner throws

```cajeta
int64 result = xs.stream().parallel().fold<int64>(
    0L,
    (acc, x) -> acc + (int64) x,
    (a, b) -> { if (a > 1_000_000_000L) throw heap OverflowException("sum too big"); return a + b; });
```

If the combiner throws at the orchestrator combine step:
- Partials remaining in the `partials[]` array drop normally via the
  array's own drop chain entry.
- Exception propagates out of the terminal call site.

Worker results that have already been combined are owned by the
running `acc`, and if `acc` is a class type, its drop chain entry
fires at unwind.

### 3.4 — Worker can't be cancelled (CPU-bound)

```cajeta
xs.stream().parallel().forEach((x) -> {
    // Tight CPU-bound loop with no await / no further next() pull
    while (true) { computeStuff(x); }
});
```

Cancellation is cooperative (Concurrency.md § Cancellation). A worker
stuck in a CPU loop never observes the cancellation flag. The scope
waits forever.

As for a diagnostic, v1 documents this restriction. v2 may add a
runtime timer that promotes long-running cancellation to a
thread-interrupt signal. That requires platform-specific signal
handling and is explicitly out of scope here.

The user-visible recommendation:

```cajeta
// Periodic yield in long compute loops makes them cancellable:
xs.stream().parallel().forEach((x) -> {
    for (int32 i = 0; i < 1000; i = i + 1) {
        computeStuff(x);
        if (i % 100 == 0) { yield(); }   // cancellation checkpoint
    }
});
```

### 3.5 — Out-of-memory at fiber-stack allocation

```cajeta
// Source is huge; driver picks 64 splits.
hugeArray.stream().parallel().count();
```

If `__cajeta_task_run` cannot allocate the per-fiber stack, about
64 KB per worker × 64 = 4 MB, due to OS memory exhaustion:

```
cajeta.error.SystemResourceException: fiber stack allocation failed
  (requested 65536 bytes; errno=ENOMEM)
  parallel driver fell back to sequential after 4 workers spawned
  at cajeta.lang.stream.parallel.ParallelDriver.drive(ParallelDriver.cajeta:N)
  at example.run(example.cajeta:5)
```

The driver catches the spawn failure, joins the workers that DID
start, runs the remaining splits on the orchestrator sequentially,
and returns the combined result. The exception is recorded as a
warning and is not raised when the fallback succeeds, because the
terminal completes correctly. If the orchestrator-side fallback ALSO
runs out of memory, for example allocating a partial buffer, THAT
exception propagates as `SystemResourceException`.

There is a decision point here. Should the OOM be raised even when
the fallback succeeds? The current draft says no, because the user
got the right answer. A debug-mode log line records the degradation
so it is visible at investigation time.

### 3.6 — `trySplit()` returns null too eagerly

```cajeta
// Custom Splittable that gives up after 1 split.
public class LazySplitter<T> implements Splittable<T> { ... }
```

```cajeta
int32 n = lazySplitter.stream().parallel().count();
```

The driver gets 1 split and runs both halves on 2 workers, the
orchestrator plus 1 spawn. There is no error. v1's split decisions
are heuristic, and trySplit returning null is the source telling the
driver "this is enough." If estimateSize was huge but the source
actually splits coarsely, the result is correct but parallelism is
reduced.

### 3.7 — `estimateSize()` lies (returns 10 when actual is 10_000)

This is the same shape as 3.6. The driver picks `ceil(sqrt(10)) = 4`
splits. trySplit then either gives the driver actual data or returns
null. If trySplit honestly halves a 10_000-element source, the 4-way
fan-out still parallelizes the workload. If trySplit returns null,
parallelism collapses. Either way the result is correct.

v1 does not validate `estimateSize` against the actual walk count.
Truthful estimates produce optimal parallelism, and lies degrade it.

### 3.8 — Source mutated during traversal

```cajeta
ArrayList<int32> list = ...;
list.stream().parallel().forEach((x) -> {
    list.add(x * 2);   // mutating the source!
});
```

This is undefined behavior, with the same contract as sequential
traversal. v1 makes no attempt to detect this. The test suite
includes a probe to verify the failure mode does not crash the
runtime. Between an assertion firing inside ArrayList and silent
corruption, we want the assertion.

A future v2 could add a generation-counter check at the splittable
sources, raising `ConcurrentModificationException`. It is listed for
completeness and is not in scope.

### 3.9 — Scheduler resource exhaustion

```cajeta
// 1024 nested parallel terminals (pathological)
for (int32 i = 0; i < 1024; i = i + 1) {
    xs.stream().parallel().count();
}
```

The scheduler's fiber pool is bounded. If exhausted, `spawn` returns
a failure:

```
cajeta.error.SystemResourceException: scheduler fiber pool exhausted
  (cap=4096 fibers; in flight=4096)
  at cajeta.lang.stream.parallel.ParallelDriver.drive(...)
  at example.run(example.cajeta:5)

  = hint: stack of in-flight scopes:
            run() @ example.cajeta:5
            <parallel worker N> @ ParallelDriver.cajeta:96 (× 1024)
```

The hint walks the per-fiber scope chain to show what is holding the
pool capacity, so you can tell a runaway recursion from a legitimate
workload that needs `parallel(Executor)` with a larger pool (v2).

### 3.10 — Lambda capturing borrowed state outliving scope (compile-time, but listed here for completeness with the runtime side)

```cajeta
async void caller() {
    int32[] xs = heap int32[1000];
    detach xs.stream().parallel().forEach((x) -> process(x));
    // ^ rejected: detach + parallel = no scope to anchor xs's borrow
}
```

Compile error from the existing `detach` capture check:

```
error[detach-borrow-capture]: cannot detach a parallel stream
       terminal — the borrow of 'xs' has no scope to anchor to
  --> example.cajeta:5:12
   5 |     detach xs.stream().parallel().forEach(...);
     |            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

  = note: parallel terminals open their own scope and join before
          returning. 'detach' opts out of scope semantics, but the
          parallel driver's workers borrow from 'xs'; without a scope
          to anchor, 'xs' could drop before the workers finish.

  = help: spawn a wrapper that owns the scope:
            spawn { xs.stream().parallel().forEach(...); };
          or transfer ownership:
            int32[] owned #= xs;
            detach owned.stream().parallel().forEach((x) -> ...);
```

---

## §4 System / edge-case scenarios

### 4.1 — Zero-element source

```cajeta
int32[] empty = heap int32[0];
int32 n = empty.stream().parallel().count();   // n == 0
```

The driver sees `estimateSize() = 0`, which is below the split
threshold, and runs sequentially. `next()` returns an empty Optional
on the first call. The result is `0`. There are no spawns and no
scope, since the driver elides the scope-open if the split count == 1.

### 4.2 — One-element source

This is the same as 4.1, and runs sequentially. The threshold elides
parallelism.

### 4.3 — Many-element / pathological-distribution source

```cajeta
// 1M element source, but estimateSize() returns 16
weirdGenerator.stream().parallel().count();
```

The driver picks 4 splits based on the estimate. Each split is
actually ~250K elements. The workers run, and the partial counts sum
correctly. It is slower than optimal, through under-parallelism, but
correct. There is no exception.

### 4.4 — Worker exits via early-return (control flow, not exception)

```cajeta
int32 first = xs.stream().parallel().findFirst((x) -> x > 100).get();
// Same shape: workers race; first hit cancels rest.
```

Workers that do not hit a match run to exhaustion on their split and
return `empty Optional`. The worker that hits cancels via the same
path as 3.1, but the cancellation trigger is "found result" rather
than "exception." The orchestrator picks the first non-empty Optional
from the partials. See §2.4 in `StreamParallelism.md` for the
order-loss caveat. Under parallel, `findFirst` is really `findAny`.

### 4.5 — Combiner with non-commutative semantics

```cajeta
// String concatenation is associative but NOT commutative
ArrayStream<String> s #= words.stream();
String joined = s.parallel()
    .fold<String>("",
        (String acc, String w) -> { return acc + w; },
        (String a, String b) -> { return a + b; });
```

`fold<R>` returns a plain `R`. On a stream temporary such as an
`ArrayList`'s `stream()`, binding a `String` or other class result to a
local is `CAJETA_ERROR_BORROW_OF_TEMPORARY`, so the stream is named
first.

Splits are disjoint contiguous index ranges, so worker N processes
indices [start_N, end_N). Per-worker accumulators are in-order
within the worker. The orchestrator's combine sweep walks partials
in worker-index order, which is source-index order. The result is the
same as sequential concatenation. The documented rule is that the
combiner must be associative. Commutativity is NOT required as long
as the source is ordered, as arrays and ArrayList are. HashMap stream
views are not ordered, so non-commutative combiners on them produce
results that vary across runs.

### 4.6 — Parallel inside async function

```cajeta
async int32 parallelInsideAsync() {
    int32[] xs = await fetchData();
    return xs.stream().parallel().reduce(0, (a, b) -> a + b);
}
```

The async fn runs on a fiber. The parallel driver opens a nested
scope and spawns workers on the same scheduler pool. The async fn
fiber blocks via `__cajeta_task_wait` until the scope joins, then
continues. There is no special handling, and it composes with the
existing async machinery.

### 4.7 — Parallel + try/catch around the terminal

```cajeta
try {
    int32 result = xs.stream().parallel().forEach((x) -> {
        if (x < 0) throw heap IllegalStateException("neg");
        process(x);
    });
} catch (IllegalStateException e) {
    log("got " + e.getMessage());
}
```

The catch sees the same exception the workers threw, re-raised at
scope exit (see §3.1). Suppressed exceptions traverse with it.

### 4.8 — Parallel terminal called from inside another parallel terminal's lambda

```cajeta
xs.stream().parallel().forEach((x) -> {
    int32 inner = ys.stream().parallel().count();   // !!!
    process(x, inner);
});
```

The inner `parallel()` runs on the outer worker's fiber. The inner
driver opens a nested scope on the same scheduler pool. If the
scheduler is busy because the outer workers saturate the cores, inner
workers queue and may stall the outer worker.

In v1 this runs correctly but may oversubscribe. The
`[parallel-nested]` warning (§2.7) catches the syntactic flatMap
form, and this dynamic form sneaks past. It is documented as a
footgun. You should hoist the inner stream to a sequential collect
outside.

### 4.9 — Cancellation during combine

```cajeta
// Terminal short-circuits via anyMatch; combine is racing with
// worker cancellation.
boolean found = xs.stream().parallel().anyMatch((x) -> x == TARGET);
```

The driver's combine for anyMatch is "OR partials." If worker A
returns true and the orchestrator is already iterating partials
when worker B's cancellation completes, the OR short-circuits on
A's true. Workers B/C/... partials are still safely accumulated
into the `partials[]` array, with no concurrent mutation, and dropped
normally at scope exit. There is no exception, and the result is
correct.

### 4.10 — Drop chain pressure under parallel

A worker's pull loop on a class-typed T pushes drop entries on its
own per-fiber chain (Concurrency.md R5 TLS promotion). With 16 workers ×
10_000 elements each, that is 160_000 push/pop pairs total. The drop
chain is a per-fiber linked list with O(1) push/pop, so it scales
linearly. The process-global drop counter is `__atomic_*_n` SEQ_CST,
and under heavy contention it is the limiting factor, since it is a
single cache line. That is acceptable for v1. The debug-mode toggle
for the counter (`--drop-chain-validate=off`) already exists per
CompilerModes.md if a tight benchmark needs it.

---

## §5 What's intentionally NOT covered (deferrals)

- **`@SuppressLint("parallel-…")` precedence rules.** These are the
  same as existing lints, with `@SuppressLint` on the enclosing
  method, class or file.
- **Custom executor / thread-pool surface.** This is the
  `parallel(Executor e)` overload. v1 always uses the built-in pool.
  It is listed as Q4 in the main doc.
- **Stream debugging — visibility into split sizes / worker timings.**
  This is useful for tuning. v2 adds `parallel(Diag d)` that captures
  split counts, per-worker durations and combine overhead. It is out
  of scope for v1.
- **GPU offload / SIMD.** This is a separate track entirely. It
  touches code generation rather than the stream surface.
- **Distributed parallel** (workers on remote machines) is not in
  scope.

---

## §6 Test plan summary

Total test count estimate for the v1 implementation phases:

| Phase | Tests | Coverage |
| ----- | ----- | -------- |
| P1 — Splittable + driver | 8 | count happy, count fallback, count empty, count 1-elem, count splittable, count non-splittable, ArrayStream.trySplit / estimateSize unit, driver-unwinds-wrappers shape pin |
| P2 — Reduce-shape terminals | 12 | reduce + anyMatch/allMatch/noneMatch (× sum / empty / no-match), forEach with AtomicInt32 |
| P3 — Cross-type combinators | 10 | fold<R>(3-arg) happy, fold<R>(2-arg) rejected, Collector combiner toList parallel, Collector no combiner rejected, mixed shapes |
| P4 — Diagnostics + source coverage | 12 | each compile-time error from §2 (6 tests), HashMap parallel keys/values/entries (3), nested-parallel warning, non-Send marker (pending the marker itself), reduce-nonzero-seed lint |
| P5 — Runtime exceptions + edge cases | 16 | every scenario in §3 (10 tests), every scenario in §4 except 4.3 (which is performance, not correctness; benchmarked separately) |

The aggregate is about 58 tests. Each test file ships incrementally
with its phase, and the design doc plus this examples doc are the
contract until the matching test lands.

---

## §7 Locked decisions (post-review 2026-05-22)

- **R1.** `findFirst` under `.parallel()` silently shifts to findAny
  semantics and emits the `[parallel-findfirst-unordered]` lint. The
  fluent surface stays intact, so you can refactor sequential to
  parallel by adding one `.parallel()` call without renaming
  terminals.

- **R2.** OOM during spawn ALWAYS raises `SystemResourceException`,
  even when the driver could fall back to sequential and complete
  correctly. The rationale is that a workload that is brittle to
  memory pressure should surface that fact at the first sign of
  trouble. Surfacing it at the fourth nested terminal, where the
  partial results compound, is too late. The exception message
  includes the spawn failure count and remaining split count so you
  can diagnose pool-size mismatches.

- **R3.** `Collector<T, R>` makes `combiner` a REQUIRED field. Every
  collector must define `(R, R) -> R combiner` at construction. The
  Collector ctor signature shifts from 2-arg `(seed, accumulator)` to
  3-arg `(seed, accumulator, combiner)`. Existing call sites,
  Collectors.toList and 4 CollectorTests sites, are updated to
  provide combiners; see the §8 migration sweep below. The
  compile-time rejection in §2.3 stays in the suite as a regression
  pin. It is now raised at the Collector ctor itself rather than at
  .parallel().collect().

- **R5.** Worker stack-trace frames are named `<parallel worker N>`
  using the 0-based split index. The driver threads the split index
  through to the spawn trampoline, and the trampoline's existing
  exception-capture machinery already prefixes the frame on throw.

## §7.5 Compiler-side issues — status

While wiring the first real fork-join path (commit `cbb79f5`), three
distinct compiler-side issues surfaced. The hard blocker is now
fixed (commit `818c19c` "Fix class→interface arg passing"), and two
spawn-related issues remain workaroundable.

1. **`spawn ClassName.staticMethod()` rejected** with
   `CAJETA_ERROR_ASYNC_R3A: spawn currently doesn't support
   instance-method calls; use a bare class-method invocation`. The
   parser treats `ClassName.` as a receiver. The workaround is to
   call the spawned worker by bare identifier from inside the same
   class (`spawn worker(args)`).

2. **`spawn worker<T>(args)` with explicit method-template args**
   rejected with `spawn currently only supports a method-call
   expression as its operand`. The inference-only form (`spawn
   worker(args)`) parses correctly. The workaround is to rely on
   inference.

3. ~~**Interface-typed parameter dispatch inside a method-templated
   static returns empty (no override dispatch).**~~ **FIXED.** Three
   interlocking compiler bugs were resolved:

   - `subtypeDistance()` only walked the `extends` chain
     (`getSuperClasses()`), so a class implementing an interface
     never matched a formal of that interface type and `resolveMethod`
     rejected the call. The BFS was extended to also walk
     `getImplementedInterfaces()`.

   - `invokeMethod`'s coercion loop passed a raw class instance
     pointer where the callee expected an interface fat-pointer body
     (`{data, vtable, kind}`). A class→interface upcast was added
     that builds the body alloca at the call site, mirroring
     `LocalVariableDeclaration`'s § S9.5.4 construction.

   - When the formal interface inherits an abstract method from a
     CLASS ancestor (e.g. `Splittable<T> extends Stream<T>` and
     `Stream<T>.next()` is the resolved target), the interface
     vtable does not hold class-method slots. A class-ancestor
     fall-through was added that loads the class vtable from the fat
     pointer's `data` slot and does the normal hash-based vtable
     lookup.

   The synthetic `TemplatedInterfaceParamProbe.cpp` test suite pins
   all five narrowed failure modes (concrete-class baseline,
   abstract-base baseline, plain-interface non-templated, templated-
   interface non-templated-static, and the original templated-static-
   templated-interface shape).

   ~~**Separate latent JIT-only issue:**~~ **FIXED.** Per-(class,
   iface) vtables were emitted with `InternalLinkage`, so
   `Linker::linkModules` renamed the donor's definition during merge
   and the consumer's extern decl, from `ensureGlobalInModule` at the
   call site, stayed unresolved. This manifested as a massive cascade
   of "Failed to materialize symbols" at JIT init. The linkage was
   switched to `ExternalLinkage`, since iface vtable names are unique
   per pair, so there is no duplicate-definition risk. The
   `parallelReduceParallelDriverDirectCall` / `parallel…ParallelDriver…`
   tests in `ParallelStreamP1Tests.cpp` now exercise direct calls
   into all four driver entry points (`reduceParallel`,
   `anyMatchParallel`, `allMatchParallel`, `noneMatchParallel`)
   end-to-end.

## §7.6 Compiler-side issues blocking real fork/join

Wiring `scope { spawn workerBody ... }` into the driver surfaces three
more compiler-side issues. Each reproduces in isolation and is
tractable as its own focused session. Until they land the driver
stays sequential, but the orchestration shape and contract are in
place.

1. **Class-typed array element reads.** `Stream<T>[] shares = ...;
   shares[i] = source.trySplit(); ...; Stream<T> share = shares[i];`
   yields a corrupted `share`, because the read returns the slot's
   l-value pointer rather than the stored class instance. This is the
   same pattern as the primitive-arg combine-loop workaround (`T p =
   partials[i]; result = fn(result, p);`), which currently dodges the
   issue by binding to a named local.

2. **`spawn` with class-instance args breaks the orchestrator's
   view.** When a spawned async function takes a class instance
   pointer, such as `Stream<T> share`, mutations the worker performs
   on the heap object do not propagate back. The spawn trampoline
   likely copies the class instance body into the capture context
   rather than aliasing through the pointer. The diagnostic is that
   `share.next()` inside the worker iterates forever, with the idx
   field never advancing, because each call reads and writes a
   different object than the orchestrator's.

3. **iface → class downcast emits an invalid bitcast.** `(Stream<T>)
   source` where source has formal type `Splittable<T>` lowers as a
   raw LLVM bitcast from the fat-pointer body to a class pointer, and
   the verifier rejects it. The cast needs to load the data slot
   out of the fat pointer and present it as the class instance.

4. **Templated-class `implements` clause with partial type-param
   passthrough hangs the compiler.** `class HashMapKeyStream<K, V>
   extends Stream<K> implements Splittable<K>` parses fine but the
   first instantiation, for example `HashMapKeyStream<Tag, int32>`,
   hangs the test binary indefinitely. The cause is either infinite
   recursion in the template instantiator or in the iface-vtable
   synthesizer when the implemented interface's type arg list is a
   proper subset of the class's type-arg list. ArrayStream<T>
   implements Splittable<T> (1-to-1) works fine, and
   HashMap*Stream<K, V> implements Splittable<K> (2-to-1) is the
   broken shape. Until this lands, HashMap.keys()/.values()/.entries()
   cannot ship parallel capability (per StreamParallelism.md P4). The
   streams stay non-Splittable and parallel-flagged terminals over
   them fall back to sequential.

The combine-loop workaround (1') stays in place. (1) and (3) are
codegen polish. (2) is the central blocker for ANY async worker
taking a class-typed arg, well beyond just the parallel driver.
(4) blocks parallel HashMap streams specifically.

## §7.7 P5 status and next-session punch list

As of 2026-05-23, items 1–4 of the original punch list landed,
and the supposed "JIT codegen loop" blocker turned out to be a
runtime segfault caused by two compiler bugs, the abstract-iface-method
return type and the class-cast bitcast, described below. Both are
fixed. The fork/join body in `reduceParallel<T>` runs end-to-end with
correct results. Workers fan out via `scope { spawn reduceWorker
<T>(...) }`, giving cooperative-fiber parallelism on today's
single-carrier scheduler. The same driver yields wall-clock
parallelism once a multi-carrier scheduler lands, with no driver
change required.

Status of each original item:

1. **§7.6 item #2 — `spawn` with class-instance args.** FIXED in
   commit `2cdd9e0`. SpawnExpression now uses loadIfLValue for
   non-Identifier l-value args (ArrayIndex GEPs, Dot GEPs) and
   captures the outer-insert-block AFTER arg evaluation, so an
   arg's own basic-block emit does not strand the post-trampoline
   restore on a terminated BB.

2. **§7.6 item #4 — `implements Splittable<K>` on a 2-type-param
   class.** FIXED for KeyStream / ValueStream cases in commit
   `b6213d1`, and FIXED for HashMapEntryStream's `Splittable<Pair<K, V>>`
   in this session. The root cause was that `CajetaClass::instantiate`'s
   placeholder-arg short-circuit, intended for `<R> #Stream<R> map`,
   returned the bare template when ALL args were placeholders. With
   `Pair<K_ph, V_ph>` that yielded bare `Pair`, then
   `Splittable.instantiate([bare-Pair])` proceeded to a real
   `Splittable<raw-Pair>` → `Stream<raw-Pair>` cascade whose
   `::reduce` body's `return this.fold(...)` instantiated
   `Stream<Pair>::fold<Pair>` with R = raw `Pair`, segfaulting at
   `R acc = seed` because a raw template has no LLVM type. The fix
   extends the short-circuit to also fire when the arg is itself a
   bare, uninstantiated template, which is the trail of a transitive
   short-circuit one level deeper. HashMapEntryStream now implements
   Splittable<Pair<K, V>> with trySplit / estimateSize and
   HashMap.entries() ships parallel capability.

3. **§7.6 item #1 — class-typed array-element-read l-value gap.**
   The documented shape (`Stream<T>[] shares; share = shares[i];
   share.next()`) now drains correctly without a named-local
   workaround. Commit `182d16f` locks the shape in via
   TemplatedInterfaceParamProbe.stdlibStreamArrayElementSharesDrain.
   It was likely fixed as a side effect of the spawn loadIfLValue,
   cast and earlier interface dispatch work cascade.

4. **§7.6 item #3 — iface → class downcast.** FIXED in commit
   `4cadf21`. CastExpression now detects iface→class downcasts, where
   the source is a CajetaClass with isInterface() and the dest is a
   non-iface CajetaClass, GEPs into the fat-pointer's data slot, and
   loads the underlying class pointer. It also late-resolves the
   child's type when codegen reaches the cast before scope
   population.

5. **Cancellation semantics (P5 proper).** LANDED for
   anyMatch / allMatch / noneMatch. Each terminal now forks via
   `scope { spawn findHitWorker / findFailWorker<T>(...) }`,
   passing a shared single-element `boolean[]` as a cooperative
   cancel flag. Workers poll the flag at the top of each loop
   iteration and bail when a sibling triggers, and the first worker
   to hit or fail flips the flag. Under today's single-carrier
   scheduler the win is that siblings short-circuit at scope join,
   with no further pulls. Once a multi-carrier scheduler lands, the
   same flag preempts in-flight worker progress. `findAny` is still
   pending. Stream<T> has the ordered `findFirst` today, and a
   `findAny` entry point plus an ordered/unordered split for
   `findFirst` are the remaining surface.

6. **parallel-reduce-nonzero-seed lint** and
   **nested-`.parallel()` warning**. These are still un-landed. The
   lint infrastructure (isLintSuppressed) is present, the emit sites
   need wiring, and the nonzero-seed analyzer needs lambda-body
   inspection.

7. **Wrapper-chain unwind**, task #54. SHIPPED.
   `xs.stream().filter(p).parallel().reduce(...)` now actually forks
   workers through `Stream.reduce → ParallelDriver.reduceParallelChain`.
   Each worker gets a freshly rebuilt wrapper chain over its share
   via `head.cloneChainOver(piece)`, and every level of the chain
   (Filter, Peek) recursively rebuilds itself bottom-up. The depth==0
   case, where the root IS the head, passes the share through
   unchanged via the `Stream<T>.cloneChainOver` base returning
   `newRoot`.

   What shipped:

     - **Unwind protocol on the base.** `Stream<T>.unwrap()` (null
       on Splittable roots, source on wrappers),
       `isStatefulWrapper()` (true on TakeStream/SkipStream),
       `splittableSize()` / `trySplitRoot()` (overridden in
       ArrayStream and the three HashMap*Streams).
     - **Recursive `cloneChainOver(#Stream<T>)`** on Stream<T> base
       (returns `newRoot` unchanged) and on FilterStream / PeekStream
       (recurse into source then wrap result in fresh
       `heap FilterStream<T>(_, pred)` / `heap PeekStream<T>(_, fn)`).
     - **`ParallelDriver.reduceParallelChain<T>(Stream<T>, T, fn)`**
       walks `unwrap()` to find the chain root and depth, throws
       `CAJETA_ERROR_STREAM_PARALLEL_REJECT_STATEFUL` on take/skip
       in the chain, pulls splits via `trySplitRoot()`, rewraps each
       via `head.cloneChainOver(piece)`, and forks via the existing
       `scope { spawn reduceWorker<T> }` shape. The return type is
       `#T` per the multi-parameter borrow-return rule.
     - **`Stream<T>.reduce` dispatches** to `reduceParallelChain<T>`
       when `isParallel` is set.
     - **Three compiler fixes** that unblocked all of the above:

       1. **MethodCall: # parameter at call site auto-deactivates
          caller's drop entry.** Before, calling a method whose
          formal was `#T param` required the caller to write `#arg`
          explicitly. Otherwise the arg's drop entry stayed active
          and the chain double-freed when the method also returned
          # ownership. Now any IdentifierExpression arg whose
          formal is `#T` is auto-deactivated at the call site.
          See `MethodCallExpression.cpp` § # transfer at call site.
       2. **LocalVariableDeclaration: borrow detection for
          non-#-returning method calls.** Before,
          `Stream<T> next = cur.unwrap();` was always treated as a
          MOVE, so a fresh drop entry got registered on `next`,
          double-freeing the borrowed reference on scope exit. Now
          the LVD resolves the called method and skips the drop
          entry when the method returns non-`#`. Mirrors the
          existing borrow detection for DotExpression and
          ArrayIndexExpression. See `LocalVariableDeclaration.cpp`
          § GAP-fix.
       3. **CajetaTask drop function: LinkOnceODR linkage.** The
          per-(T) `__cajeta_task_Task_T_drop` function was emitted
          with `ExternalLinkage`, so a stdlib JIT module defining
          it would collide with any user module whose own `async`
          functions returned `Task<T>` for the same T. It was
          switched to `LinkOnceODRLinkage` + comdat so the linker
          merges duplicate definitions. This unblocks `Stream.reduce`
          dispatch, which would otherwise pull `Task<T>` into the
          stdlib module from every test's perspective.

   What did NOT ship in this pass:

     - **Same dispatch flip on `anyMatch / allMatch / noneMatch /
       forEach`.** This shipped in a follow-up pass (see § 7.8).
     - **Type-changing wrappers (MapStream<U,T>, FlatMapStream<U,T>).**
       The unwind layer is still element-type-uniform. MapStream's
       T→R boundary breaks the lambda/closure composition shape
       used by the current cloneChainOver design. Lifting needs an
       Object-erasure layer or `<?>` wildcards, and CajetaType.cpp:454
       rejects wildcards today.
     - **`fold<R>(seed, fn)` under `.parallel()`.** See errata
       *Stream parallelism — fold(seed,fn) needs combiner overload
       first* below.

## §7.8 Wrapper-aware predicate + forEach terminals

This section lands the same dispatch flip on `anyMatch / allMatch /
noneMatch / forEach` that § 7.7 landed for `reduce`. The shape is the
same across all four:

```
public boolean anyMatch((T) -> boolean pred) {
    if (this.isParallel) {
        return ParallelDriver.anyMatchParallelChain<T>(this, pred);
    }
    // existing sequential walk
}
```

On the driver side, `anyMatchParallelChain / allMatchParallelChain /
noneMatchParallelChain / forEachParallelChain` each walk
`head.unwrap()` to find the chain root, reject stateful
intermediates (take/skip in chain → REJECT_STATEFUL), pull splits
via `cur.trySplitRoot()`, rewrap each share via
`head.cloneChainOver(piece)`, and fork via
`scope { spawn findHitWorker / findFailWorker / forEachWorker }`.
The three predicate terminals reuse the existing `findHitWorker` and
`findFailWorker` and their shared `boolean[1]` cancel flag. forEach
gets a new no-cancel `forEachWorker` since it visits every element.
The tail walks `head.next()` on the orchestrator after scope join.

Tests (`ParallelStreamP1Tests.parallel{AnyMatch,AllMatch,NoneMatch,
ForEach}DispatchesThroughFilter`) pin the fluent dispatch through
filter chains, and `parallelAnyMatchRejectsStatefulInChain` pins the
take/skip reject path.

## §7.9 Errata — deferred concerns

These items surfaced during wrapper-aware terminal work and were
deferred. Each is named `Stream parallelism — <concern>`.

### Stream parallelism — fold(seed,fn) 2-arg call-site lint

This is resolved by the 3-arg overload. § 7.10 lands the
parallel-friendly `fold<R>(seed, fn, combiner)` overload and
dispatch. The remaining follow-up is the call-site lint
`CAJETA_ERROR_STREAM_PARALLEL_FOLD_NO_COMBINER` (per § 2.2) that
fires when 2-arg `fold` is invoked on a parallel-flagged stream.
Today the 2-arg overload silently runs sequentially even on a
parallel head, since `Stream<T>.fold<R>(R, (R,T)->R)` has no parallel
branch, by design. The lint sits at semantic-analysis call sites
rather than the runtime. It was not pursued in this session.

### Stream parallelism — collect parallel path

This is RESOLVED, and "collect needs supplier" below has the final
shape. `collect<R>(Collector<T, R>)` now routes through
`collectParallelChain` under `.parallel()` using the collector's
`supplier` and `combiner`, and the sequential path calls
`fold(c.supplier(), c.accumulator)`. The originally planned `seed`
field became a per-worker `supplier`.

### Stream parallelism — `findFirst` under parallel becomes findAny

Per § R1, `findFirst` under `.parallel()` silently shifts to
findAny semantics and emits the `[parallel-findfirst-unordered]`
lint at the call site. Today findFirst is sequential under
.parallel(), with no dispatch, and the parallel path wants a
`findAnyParallelChain` worker that signals on the first match. It
returns the found element rather than a boolean, so it is distinct
enough from anyMatch to be its own dispatch.

### Stream parallelism — anyMatch result correlation in dispatch tests

The new fluent-dispatch tests for anyMatch/allMatch/noneMatch all
pass with EITHER the sequential walk or the parallel fork, because
the boolean results are identical per the associativity and
commutativity contract. The TDD signal that actually gated the
implementation was `parallelAnyMatchRejectsStatefulInChain`, since
only the parallel dispatch path throws REJECT_STATEFUL on a
stateful-in-chain. A stronger "did the parallel path run?" assertion
would need either a side-channel such as a worker-count counter or a
deliberately non-commutative predicate that observes intermediate
state. It was not pursued because the existing tests plus the
stateful reject pin enough of the contract.

### Stream parallelism — collect needs supplier

`Collector<T, R>` carries a single `seed` instance, so for a mutable
R like `ArrayList`, every parallel worker would alias that one
instance and race on `add` and drop bookkeeping. The obvious "route
collect through the 3-arg fold" approach was tried against
`Collectors.toList<int32>` on 100 elements. The JIT'd worker pulled
the shared list, `acc.add(x)` raced across workers, and malloc
reported "mismatching next->prev_size" and aborted. The fix is to
give `Collector<T, R>` a `supplier: () -> R` field, Java's
`Collector.supplier()`, so each worker gets a fresh accumulator. It
touches:

  - `cajeta.collection.Collector` gains a `supplier: () -> R` field
    and a 4-arg ctor, or replaces seed with supplier.
  - `ParallelDriver.foldParallelChain` calls `supplier()` once per
    worker partial-slot init instead of storing `seed`.
  - `cajeta.collection.Collectors.toList<T>` supplies
    `() -> heap ArrayList<T>()` alongside the existing accumulator
    and combiner.
  - `Stream<T>.collect<R>` passes `c.supplier` through to the
    parallel driver, and the sequential path stays on the
    seed-or-first call.

This is RESOLVED. The `supplier: () -> #R` field landed on
`Collector<T, R>`, with the 3-arg ctor `(supplier, accumulator,
combiner)`, `Collectors.toList<T>` supplies `() -> heap ArrayList<T>()`,
and `Stream<T>.collect<R>` now dispatches to
`ParallelDriver.collectParallelChain<T, R>(this, c.supplier,
c.accumulator, c.combiner)` under `.parallel()`. The
`COLLECT_NO_SUPPLIER` reject is gone. Parallel collect works
end-to-end, and `.sequential().collect(...)` is no longer required.

### Stream parallelism — heap-class-array writes from spawned workers

This surfaced in § 7.11 (findFirst → findAny). Writing a freshly
allocated `heap Optional<T>(...)` directly into a shared
`Optional<T>[1]` slot from inside a `spawn`-ed worker template hung
the test indefinitely. It was worked around by storing the raw
element `T` into `T[1]` from the worker and constructing the Optional
on the orchestrator after scope-join. The same pattern would also
bite any future "store fresh heap allocation into shared class-typed
slot" work. `collect`-style mutable accumulators, eg. ArrayList<T>
combiners, would need a fix here to work in-place. It is likely a
drop-chain or ownership-tracking interaction with the async worker's
epilogue. It is not bisected yet because the T[1] workaround is clean
and sufficient for findAny.

### `findAny` result correlation in dispatch tests

This has the same TDD weakness as anyMatch (§ 7.9 entry above). The
three "happy path" findFirst-parallel tests (`ReturnsAMatchingElement`,
`EmptyOnNoMatch`, `ThroughFilter`) all pass against EITHER the
sequential walk or the parallel fork, because sequential happens to
return a valid findAny answer, since any "first match" is in the
valid match set. Only `RejectsStatefulInChain` actually gates the
parallel dispatch path. A side-channel counter or a deliberately
non-commutative predicate could distinguish them.

The "JIT codegen loop" diagnosis was incorrect. The bug was a runtime
SEGFAULT rather than a compile hang. gtest cannot flush its
[ FAILED ] line before the binary aborts, so the test appeared to
hang. There were two root causes, both fixed:

  1. **Abstract iface method's call-site return type was wrong.**
     `Method::generatePrototype`'s abstract-method branch built
     llvmFunctionType from `returnType->getLlvmType()` directly,
     which for a class-typed return resolves to the class body
     struct (e.g. `{ ptr, i1 }` for Stream<int32>) rather than
     `ptr`. The concrete implementer's signature uses `ptr`, because
     the non-abstract path applies the return-by-pointer rule. The
     mismatched call signature stored a 16-byte struct into an
     8-byte alloca, overflowing the stack. It was fixed by mirroring
     the non-abstract path's return-by-pointer adjustment in the
     abstract branch.

  2. **Cast `(Stream<T>) source.trySplit()` emitted invalid
     bitcast.** With (1) fixed, the trySplit return is correctly
     typed `ptr`. The cast destination's `getLlvmType()` is the
     class body struct, and `CastExpression::generateCode` fell
     through to `CreateBitCast(ptr, struct)`, which JIT-verify
     rejects in opaque-pointer LLVM. It was fixed by recognising that
     class-typed destinations store as `ptr` at runtime, so a
     `ptr → class` cast is a no-op at the LLVM level.

Ownership transfer into a class-typed array slot now lands.
`BinaryOpExpression`'s ASSIGN path, when the LHS is an
`ArrayIndexExpression` with a class-storing element type and the
RHS is an `IdentifierExpression`, looks up the source local's
field and deactivates its drop entry via
`__cajeta_drop_mark_inactive`, the same machinery as the lambda
`#capture` transfer. The reduceParallel driver writes the
natural pattern (`Stream<T> piece = source.trySplit(); shares[si]
= piece;`) without leaking or dangling. `markMoved` is
intentionally NOT called. The source name is often re-assigned in
the next iteration, as with the trySplit loop's fresh `piece` each
turn, or, for parameter sources, read again in unrelated paths, as
with HashMap.put's `this.keys[i] = key` followed by future probes.
The drop-deactivation half is necessary and sufficient.

## §7.10 fold(seed, fn, combiner) — parallel cross-type fold

This section lands the 3-arg `fold<R>` overload and driver entry.
The dispatch shape is the same as `reduce` and the predicate
terminals. `Stream<T>.fold<R>` consults `isParallel`, and the
driver's `foldParallelChain<T, R>` walks
`head.unwrap()`, rejects stateful intermediates (take/skip →
REJECT_STATEFUL), pulls splits via `cur.trySplitRoot()`, rebuilds
each share via `head.cloneChainOver(piece)`, and forks workers via
`scope { spawn foldWorker<T, R> }`. The combiner only fires on the
orchestrator at scope-join, merging `partials[0..shareCount]`
left-to-right with `seed` as the initial accumulator.

Cross-type R is the headline payoff. An int32 stream can be summed
into int64, a T-stream `collect`ed into ArrayList<T>, an any-T-stream
concatenated into String, and so on. The 2-arg `fold<R>` overload
stays unchanged and sequential-only, and the 3-arg version is the
parallel-friendly path.

```cajeta
public final R fold<R>(R seed, (R, T) -> R fn, (R, R) -> R combiner) {
    if (this.isParallel) {
        return ParallelDriver.foldParallelChain<T, R>(this, seed, fn, combiner);
    }
    // existing sequential walk
}
```

On the driver side, the per-worker drain is template-on-(T, R),
Cajeta's first multi-param method-level template in the stdlib:

```cajeta
public static async int32 foldWorker<T, R>(
    Stream<T> share, int32 slot, R[] partials, (R, T) -> R fn) { ... }
```

Tests (`ParallelStreamP1Tests.foldCombiner{SeqI32ToI64,Parallel
DirectArrayHead,ParallelThroughFilter,RejectsStatefulInChain}`)
pin: sequential overload resolution, parallel direct-array head,
parallel through filter chain, and stateful-reject.

**Contract** (per Java + StreamParallelism.md § Per-terminal rules):
  - `combiner` MUST be associative.
  - `seed` MUST be the identity for both `fn` and `combiner`:
    `combiner(seed, fn(seed, t)) == fn(seed, t)` for every t.

This unblocks the next errata item, parallel `collect`, which routes
through `fold(seed, fn, combiner)` once `Collector<T, R>` grows the
combiner field (§ 8 migration sweep).

## §7.11 findFirst → findAny under parallel

Per § R1 and § Per-terminal rules, `findFirst` under `.parallel()`
semantically shifts to findAny. Encounter order across split workers
is not preserved, so "first in source order" is not meaningful. The
dispatch flip lives entirely on the existing `findFirst` method:

```cajeta
public Optional<T> findFirst((T) -> boolean pred) {
    if (this.isParallel) {
        return ParallelDriver.findAnyParallelChain<T>(this, pred);
    }
    // sequential walk unchanged
}
```

On the driver side, `findAnyParallelChain<T>` follows the
now-standard chain-aware shape. It walks unwrap, rejects stateful,
rebuilds each share via `head.cloneChainOver(piece)`, and forks via
`scope { spawn findAnyWorker<T> }`. The worker stores the raw
hit-element into a shared `T[1] result` slot and flips `boolean[1]
hit`, and the orchestrator builds the `Optional<T>` wrapper after
scope-join.

T[1] was chosen over Optional<T>[1] for a reason. The first cut wrote
a freshly allocated `heap Optional<T>(...)` directly into a shared
`Optional<T>[1]` from inside the worker. The targeted test hung
indefinitely, with no failure line and no crash, in a pthread wait
inside the worker fiber. Class-typed array writes from heap-new
inside a spawned template have not been validated end-to-end.
Storing the raw T sidesteps that codepath. Workers do a plain
element-type write, a primitive copy for T=int32 and a pointer store
for class T, and the single `heap Optional<T>(true, result[0])`
happens on the orchestrator after the scope joins. It is
errata-worthy as a follow-up ("Stream parallelism — heap-class-array
writes from spawned workers").

The tests are `ParallelStreamP1Tests.parallelFindFirst{ReturnsAMatching
Element, EmptyOnNoMatch, ThroughFilter, RejectsStatefulInChain}`.
The first three are correctness-preserving across sequential and
parallel paths, since any match in {51..100} is valid. The
stateful-reject test is what gates the parallel dispatch. This is
the same TDD weakness as the predicate terminals; see the § 7.9
errata.

## §7.12 2-arg fold + collect: parallel-reject

There are two runtime rejects mirroring the take/skip pattern:

  - `Stream<T>.fold<R>(R seed, (R, T) -> R fn)` (2-arg) throws
    `CAJETA_ERROR_STREAM_PARALLEL_FOLD_NO_COMBINER` on a parallel
    head, because no combiner means no way to merge partials. You
    opt into parallel reduction via the 3-arg `fold<R>(seed, fn,
    combiner)` overload (§ 7.10) or escape via `.sequential()`.
  - `Stream<T>.collect<R>(Collector<T, R>)` no longer rejects. The
    `COLLECT_NO_SUPPLIER` gap (§ 7.9 errata) is closed. `Collector`
    now carries a per-worker `supplier`, so parallel collect runs
    end-to-end. Only the 2-arg `fold` reject above remains.

The 2-arg-fold reject is a runtime throw, a `cajeta.error.Exception`
with the documented error ID. It cannot be a compile-time lint,
because the parallel flag is a runtime property. The closest static
lint would be syntactic, asking whether the chain contains
`.parallel()` literally, which Cajeta's semantic phase does not
surface yet. So the runtime throw aligns with the existing take/skip
pattern.

Tests pin the 2-arg-fold throw and the collect path:
`twoArgFoldOnParallelStreamRejects` and the parallel-collect suite.

## §8 Migration sweep (Collector R3)

Before implementation, the existing `Collector<T, R>` ctor is 2-arg.
The R3 decision adds a required 3rd `combiner` parameter. The touched
sites:

| File | Change |
| ---- | ------ |
| `runtime/src/cajeta/collection/Collector.cajeta` | add `combiner` field + 3-arg ctor; remove 2-arg ctor |
| `runtime/src/cajeta/collection/Collectors.cajeta` | `Collectors.toList<T>()` passes `(a, b) -> a.appendAll(b)` as combiner |
| `runtime/src/cajeta/collection/ArrayList.cajeta` | add `appendAll(ArrayList<T> other): void` if not present |
| `test/parser/CollectorTests.cpp` | 4 ctor calls get a combiner (sum: `(a, b) -> a + b`) |
| `test/parser/LambdaL2Tests.cpp` | 1 call updated (line 209+ comment ref) |

Migration goes in the P1 prep commit so subsequent parallel work
sees the new shape from the start.
