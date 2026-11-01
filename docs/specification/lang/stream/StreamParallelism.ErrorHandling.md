# Stream Parallelism — Error Handling

Companion to `StreamParallelism.md` and `StreamParallelism.Examples.md`.
This document specifies how exceptions thrown inside stream operations
behave, both sequential and parallel, and the user patterns that let a
single worker's failure leave the rest of the computation running.

The policy itself is short. Stream operations do not get an
error-handling sub-DSL. This document walks through what that
means concretely, what is locked, and how to write the patterns that
look like "tolerance" without bolting a new mechanism onto the
terminal surface.

The error model this builds on is `ErrorModel.md`. The scope and
task semantics are `Concurrency.md` and `ErrorModel.md § Async / fiber
integration`. Read both before extending this spec.

---

## §1 Position

Java's `Stream` API treats exceptions as a foreign body. Checked
exceptions cannot flow through a `Function<T, R>` without ugly
sneaky-throws workarounds, parallel streams wrap failures in
`RuntimeException`, and an "ErrorHandler" decorator chain in the
RxJava style of `.onErrorReturn(...)` / `.onErrorResume(...)` became
the de facto pattern. Cajeta rejects that whole class of API.

The core position, already in `ErrorModel.md § Streams`, is:

  - Lambdas in stream operations throw like any other code.
  - The exception propagates through the pipeline the same way it
    propagates through a function call.
  - The terminal is where the exception surfaces to the caller.
  - The caller wraps the terminal in `try/catch` if they want recovery.
  - There are no decorator chains, no Try/Result wrapping mandated by the
    pipeline, and no catch-by-step such as `.map(λ).catch(IOException, λ)`.
  - There is no `AggregateException` at the stream layer.

The pipeline is a sequence of transformations applied to elements.
A region-of-code idiom such as try/catch does not fit that shape, and
the existing language-level error handling does the job at either end,
in the lambda body or at the terminal call site.

---

## §2 Default behavior

### §2.1 Sequential

```cajeta
try {
    int32 sum = xs.stream()
                  .map(x -> riskyTransform(x))   // can throw IOException
                  .reduce(0, (a, b) -> a + b);
} catch (IOException e) {
    log.warn("transform failed: " + e.message);
}
```

What happens:
- `reduce` pulls `next()` from the chain.
- `MapStream.next()` invokes `riskyTransform(x)`.
- `riskyTransform` throws `IOException`.
- Exception unwinds through `MapStream.next()`, the reduce loop, the
  terminal itself, and lands in the caller's `catch`.
- All owned locals along that unwind drop normally. Cajeta's
  drop-chain on the throw path runs the same as on return, per
  `ErrorModel.md § Drop chain on the throw path`.

Per-element recovery is per-element `try/catch` inside the
lambda:

```cajeta
xs.stream()
  .map(x -> {
      try { return riskyTransform(x); }
      catch (IOException e) { return -1; }
  })
  .reduce(0, (a, b) -> a + b);
```

The lambda owns the catch. No exception ever leaves the worker
context, nothing propagates up, and the pipeline sees only successful
elements, some with sentinel values.

### §2.2 Parallel

Parallel behavior mirrors `scope`'s join semantics from `ErrorModel.md § scope and
exception escalation (R5-D)`. Concretely:

1. A worker fiber's lambda throws.
2. The throw unwinds the worker's frames; the worker's owned locals
   drop on the unwind (same machinery as sequential).
3. The fiber trampoline catches the exception at the worker's
   outermost frame and stores it in the worker's `Task<T>.exception`
   slot. The worker marks itself done.
4. The orchestrator's `scope { ... }` join-pass walks all spawned
   children. The first child found with a non-null exception
   slot is the trigger. The walk order is worker-slot order, which is
   deterministic.
5. Every still-running sibling has its scope cancel flag set, and each
   surfaces a `CancellationException` at its next pull-loop
   checkpoint. Siblings unwind and drop their locals.
6. The orchestrator awaits every child's terminal state, then
   re-raises the trigger's original exception at the scope's containing
   frame, which is the parallel terminal. The exception is unwrapped, with
   no `ExecutionException` wrapper and no `CancellationException` wrapper.
7. The terminal's caller catches normally:

```cajeta
try {
    int32 sum = xs.stream()
                  .parallel()
                  .map(x -> riskyTransform(x))   // any worker can throw
                  .reduce(0, (a, b, c) -> a + b, (a, b) -> a + b);
} catch (IOException e) {
    log.warn("at least one worker failed: " + e.message);
}
```

Other workers' exceptions are lost, and only the trigger surfaces.
This is intentional. Gathering every worker's outcome is what § 3.2
("keep the error as data") is for.

`CancellationException` is a local concern, and it does not escape
the scope. If a sibling has a `try { ... } catch (CancellationException
e) { ... }` around its own work, that catch sees it and can use
`e.getCause()` to inspect the trigger. Otherwise the cancellation
unwinds the sibling silently, the drops fire, and the sibling's slot finishes.

### §2.3 Drop chain on the throw path

`ErrorModel.md § 201` covers this for general code. The parallel-
specific notes are:

- Each worker fiber has its own drop chain (per-fiber linked list).
- On a worker throw, the worker's chain unwinds top-down to the spawn
  frame.
- The fiber trampoline runs the spawn-frame drop entries before storing
  the exception in the Task slot. The formal-parameter slots receive their
  owned values, and those drop here.
- Sibling workers cancelled by scope unwind their own chains in the
  same way.
- The orchestrator's drop chain is unaffected until the scope's
  closing brace. On re-raise of the trigger, the orchestrator's
  locals drop as part of the terminal's own unwind.

The result is that an exception in parallel is no leakier than an
exception in sequential. Every fiber cleans up after itself.

### §2.4 Worker-side cancellation cooperative-checkpoint

A cancelled sibling sees the cancel flag at its next pull-loop
iteration. The `while (o.isPresent())` guard checks the scope's
flag implicitly via `next()` returning the cancellation signal, or
the worker explicitly polls, as `Concurrency.md § Cooperative
cancellation` describes. Workers that do not have a checkpoint loop
will not observe the cancel until they next reach one. That is rare for
streams, because every stream worker pulls from `next()`.

The single-carrier scheduler today serializes worker execution, so
the "cancel observation latency" is the work between checkpoints
of a single fiber. It is not parallel-runtime latency. Multi-carrier
will need TLS-promoted `__cajeta_exc_top` and per-fiber cancel-flag
hot-paths (`ErrorModel.md § TLS hardening`), and the user-visible
contract above does not change.

---

## §3 User patterns for tolerance

The locked default is fail-fast. The patterns below let you choose
tolerance at the layer where it makes sense, whether the lambda, the
element shape or the stdlib helper. The terminal interface stays
unchanged.

### §3.1 Pattern A — lambda-local try/catch

This is the simplest answer to "don't let one exception kill the run":

```cajeta
int32 sum = xs.stream()
              .parallel()
              .map(x -> {
                  try { return riskyTransform(x); }
                  catch (IOException e) { return 0; }   // sentinel
              })
              .reduce(0, (a, b) -> a + b, (a, b) -> a + b);
```

The worker never throws, the scope sees no exception, and there is no
cancellation. The sum is computed over the successful elements plus the
sentinels. Use this when:
  - You can pick a sentinel that's harmless under your accumulator
    (0 for sum, identity for product, "" for string-concat).
  - You don't need to know which elements failed.

### §3.2 Pattern B — keep the error as element data

Carry both outcomes through the pipeline as the element type:

```text
class Outcome<T> {
    public T value;
    public Exception error;
    public boolean ok;

    public Outcome(T v) { this.value = v; this.ok = true; this.error = null; }
    public Outcome(Exception e) { this.value = null; this.ok = false; this.error = e; }

    public static <T> Outcome<T> tryRun(() -> T fn) {
        try { return heap Outcome<T>(fn()); }
        catch (Exception e) { return heap Outcome<T>(e); }
    }
}

ArrayList<Outcome<Score>> all #= urls.stream()
    .parallel()
    .map(u -> Outcome.tryRun(() -> fetchScore(u)))
    .collect(Collectors.toList<Outcome<Score>>());

// downstream: a stream is single-pass, so split the collected list
ArrayList<Score> ok = heap ArrayList<Score>();
ArrayList<Exception> errs = heap ArrayList<Exception>();
for (int32 i = 0; i < all.count(); i = i + 1) {
    Outcome<Score> o = all.get(i);
    if (o.ok) { ok.add(o.value); } else { errs.add(o.error); }
}
```

`collect` returns an owned `#R`, so the pipeline fits in one
statement. The streams it builds are freed at the end of that statement,
and the collected list outlives them. The elements are another matter.
`Collectors.toList` keeps borrows of class elements, and nothing owns an
`Outcome` that `map` creates once the statement ends. So today `all`
holds freed nodes, and reading them returns garbage. Collect class
elements only when their source owns them.

`Outcome<T>` is user-defined and lives outside the stdlib. The stream layer
does not impose it, and it imposes nothing about element types other
than `T`. This is what we mean by "we do not wrap into Try/Result."

Use this when:
  - You want every failure to surface, beyond the first.
  - The downstream wants both successes and errors.
  - The element count is bounded enough that materializing the full
    set is reasonable.

For "I only want the count of failures" or "I want to know the
total but tolerate misses," `Outcome<T>` is heavier than a sentinel
plus a counter (§ 3.4). Pick the right tool.

### §3.3 Pattern C — opt-in stdlib helpers

When the lambda-local try/catch is verbose enough to obscure intent,
the stdlib supplies named operators that codify a recovery shape:

```cajeta
public final #Stream<R> mapOrSkip<R>((T) -> #R fn);
public final #Stream<R> mapOrFallback<R>((T) -> #R fn, R fallback);
public final #Stream<R> mapOrLog<R>((T) -> #R fn,
                                    (T, Exception) -> void logger);
```

Behavior:
- **mapOrSkip** drops elements whose `fn` throws `RecoverableException`,
  so the stream shortens.
- **mapOrFallback** substitutes the user-supplied `fallback` for elements
  whose `fn` throws `RecoverableException`, so the stream length is
  unchanged.
- **mapOrLog** passes elements whose `fn` throws `RecoverableException`
  to `logger(elem, exception)` and then drops them.

`UnrecoverableException`, such as an assertion failure or OOM, is never
caught by these helpers, so the alarm contract from `ErrorModel.md §
The hierarchy` is preserved. You can still catch it at the
terminal or let the runtime's default catch handle it.

These are not magic. They expand to the same lambda-local try/catch
pattern, and the operator name is the documentation that "failures here
are being suppressed by design." A reader does not have to scan a
trailing `.onError(...)` decorator to know.

```cajeta
int32 sum = urls.stream()
                .mapOrFallback(u -> fetchScore(u), 0)
                .reduce(0, (a, b) -> a + b);
```

These helpers catch inside the lambda body, so the parallel driver
sees no worker exceptions and no cancellation. They are parallel-clean.

There is a type-changing wrapper caveat. Like `Stream<T>.map<R>`, these
helpers are type-changing wrappers (`Stream<T> -> Stream<R>`). Under
`.parallel()` the wrapper-chain walk stops at the type-flip, so the
chain falls back to sequential. That is the same documented limitation as the
other type-changing intermediates (`StreamParallelism.Examples.md §
7.9`). Tests for this surface live in `StreamIntermediateTests.cpp`
as `mapOr{Skip,Fallback,Log}*`.

### §3.4 Pattern D — threshold-based abort

This pattern tolerates up to N failures, then fails the whole run. It
uses a shared counter inside the lambda:

```cajeta
int32[] failCount = heap int32[1];
int32 threshold = 5;

int32 sum;
try {
    sum = urls.stream()
              .parallel()
              .map(u -> {
                  try { return fetchScore(u); }
                  catch (IOException e) {
                      failCount[0] = failCount[0] + 1;
                      if (failCount[0] > threshold) {
                          throw heap TooManyFailuresException(failCount[0], threshold);
                      }
                      return 0;
                  }
              })
              .reduce(0, (a, b) -> a + b, (a, b) -> a + b);
} catch (TooManyFailuresException e) {
    log.warn("aborted after " + e.count + " failures");
    sum = -1;
}
```

When the counter crosses the threshold inside any worker, that
worker throws `TooManyFailuresException`. The throw triggers the
locked default. That worker's exception becomes the trigger,
siblings cancel, the terminal re-raises, and the caller's `catch`
handles the abort.

There is a single-carrier note. The `failCount[0] = failCount[0] + 1` is
not atomic. On the single-carrier scheduler today, workers
serialize, so the increment is observably correct. When multi-
carrier lands, the counter wants `AtomicInt32` (Concurrency.md §
Atomics). The pattern does not change, and only the type does.

Use this when:
  - You want a circuit-breaker beyond suppression.
  - The threshold is small, because close behavior dominates the spec
    more than any "tolerate the noise" case.

---

## §4 Combiner / accumulator exceptions (parallel terminals)

The combiner of a parallel reduce / fold runs on the
orchestrator after the scope joins. If the combiner throws:

  - Worker tasks have already completed, some successfully and others
    cancelled-then-completed.
  - The orchestrator's local exception unwinds normally. The
    partial values still live in `partials[]` and drop as part of
    the orchestrator's unwind.
  - The caller sees the combiner's exception, and no worker
    exception.

This is not AggregateException territory. If a worker had
thrown earlier in the same call, the scope's join-pass would have
re-raised the trigger before the combiner ran, and the combiner
would never be reached. The combiner-throws case is post-success only.

```text
try {
    ArrayStream<T> s #= xs.stream();
    R result = s.parallel()
                .fold(seed, (acc, x) -> riskyAcc(acc, x), (a, b) -> riskyCombine(a, b));
} catch (Exception e) {
    // Could be from any worker's riskyAcc OR the orchestrator's
    // riskyCombine. The user catches at the terminal regardless.
}
```

---

## §5 What we explicitly do NOT do

This section re-states ErrorModel.md § Streams' negatives in the parallel context.

  - **No decorator chains.** There is no `.onErrorReturn(...)`,
    `.onErrorResume(...)` or `.retry(n)`. They live in the wrong
    place, separated from the producing operation that failed,
    and they flatten per-step context.
  - **No stream-level Try/Result wrapping.** The pipeline does not
    mandate `Try<T>` or `Result<T, E>`. Users who want that shape
    define their own `Outcome<T>` (§ 3.2), and the pipeline is
    type-agnostic.
  - **No catch-by-step syntax.** `.map(λ).catch(IOException, λ).filter(λ)`
    treats the pipeline as a try/catch region, and it is not one.
  - **No `AggregateException` at the terminal.** First-throw wins
    in the scope's join pass, and other workers' exceptions are lost.
    A future opt-in `scope.collectAll() -> List<Result<T>>` may give
    a per-task outcome list at the scope layer, which is deferred per
    `ErrorModel.md § 197`.
  - **No `.parallel(maxFailures=N)` knob.** Threshold-based abort
    lives in user-authored lambdas (§ 3.4). The reason is the same one
    that rejects the decorator chain. Lambda-local code knows what
    "failure" means, and the terminal does not.
  - **No checked-exception cascade.** `throws` clauses on lambdas
    are documentation, per the language's overall error position, and
    the compiler does not enforce them.

---

## §6 Cross-references

  - `ErrorModel.md § The hierarchy` covers RecoverableException and
    UnrecoverableException, and what `catch (Exception e)` catches.
  - `ErrorModel.md § Async / fiber integration` covers the Task<T>.exception
    slot, the fiber trampoline, and scope escalation.
  - `ErrorModel.md § Streams (forward-looking)` is the original
    locus of the stream policy. The detail here supersedes it for
    parallel behavior, and the position statements there still hold.
  - `StreamParallelism.md § Goals (item 4)` states the cooperative
    cancellation contract.
  - `StreamParallelism.md § Locked decisions` covers findFirst →
    findAny under parallel, the fail-fast default, and combiner-required.
  - `StreamParallelism.Examples.md § 7.9` lists errata items that
    interact with error handling, such as collect needing a supplier.
  - `Concurrency.md § Cooperative cancellation` and `Concurrency.md § Atomics`
    are the underlying machinery that the patterns here rely on.

---

## §7 Summary table

| Goal | Pattern | Mechanism |
| ---- | ------- | --------- |
| Suppress all failures, use sentinel | § 3.1 lambda try/catch | catch in lambda body, return sentinel |
| Suppress, named operator | § 3.3 stdlib helper | mapOrSkip / mapOrLog / mapOrFallback |
| Keep both outcomes, partition downstream | § 3.2 Outcome<T> | user-defined element type |
| Abort after N failures | § 3.4 threshold | shared counter + lambda throw |
| Default: stop on first failure | § 2.2 fail-fast | scope's first-throw-wins (locked) |
