# Stream Parallelism — Error Handling

Companion to `StreamParallelism.md` and `StreamParallelism.Examples.md`.
Specifies how exceptions thrown inside stream operations behave —
sequential and parallel — and the user patterns that let a single
worker's failure not abort the whole computation.

The policy itself is short: **stream operations don't get an
error-handling sub-DSL.** This document walks through what that
means concretely, what's locked, and how to write the patterns that
look like "tolerance" without bolting a new mechanism onto the
terminal surface.

The error model this builds on is `ErrorModel.md`. The scope and
task semantics are `Thread.md` and `ErrorModel.md § Async / fiber
integration`. Read both before extending this spec.

---

## §1 Position

Java's `Stream` API treats exceptions as a foreign body — checked
exceptions can't flow through a `Function<T, R>` without ugly
sneaky-throws workarounds, parallel streams wrap failures in
`RuntimeException`, and an "ErrorHandler" decorator chain
(RxJava-style `.onErrorReturn(...)` / `.onErrorResume(...)`) became
the de facto pattern. Cajeta rejects that whole class of API.

The core position (already in `ErrorModel.md § Streams`):

  - Lambdas in stream operations throw like any other code.
  - The exception propagates **through the pipeline** the same way it
    propagates through a function call.
  - The **terminal** is where the exception surfaces to the caller.
  - The caller wraps the terminal in `try/catch` if they want recovery.
  - **No** decorator chains, no Try/Result wrapping mandated by the
    pipeline, no catch-by-step (`.map(λ).catch(IOException, λ)`).
  - **No** `AggregateException` at the stream layer.

The pipeline is a sequence of transformations applied to elements,
not a region of code. A region-of-code idiom (try/catch) doesn't fit
the shape; existing language-level error handling does the job at
either end (the lambda body or the terminal call site).

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
- All owned locals along that unwind drop normally (Cajeta's
  drop-chain on the throw path runs the same as on return —
  `ErrorModel.md § Drop chain on the throw path`).

Per-element recovery is per-element `try/catch` **inside the
lambda**:

```cajeta
xs.stream()
  .map(x -> {
      try { return riskyTransform(x); }
      catch (IOException e) { return -1; }
  })
  .reduce(0, (a, b) -> a + b);
```

The lambda owns the catch. No exception ever leaves the worker
context; nothing propagates up; the pipeline sees only successful
elements (some with sentinel values).

### §2.2 Parallel

Mirrors `scope`'s join semantics from `ErrorModel.md § scope and
exception escalation (R5-D)`. Concretely:

1. A worker fiber's lambda throws.
2. The throw unwinds the worker's frames; the worker's owned locals
   drop on the unwind (same machinery as sequential).
3. The fiber trampoline catches the exception at the worker's
   outermost frame and stores it in the worker's `Task<T>.exception`
   slot. The worker marks itself done.
4. The orchestrator's `scope { ... }` join-pass walks all spawned
   children. The **first** child found with a non-null exception
   slot is the **trigger** (walk order is worker-slot order —
   deterministic).
5. Every still-running sibling has its scope cancel flag set; each
   surfaces a `CancellationException` at its next pull-loop
   checkpoint. Siblings unwind, drop their locals.
6. The orchestrator awaits every child's terminal state, then
   re-raises the **trigger's original exception** — unwrapped, no
   `ExecutionException` wrapper, no `CancellationException` wrapper —
   at the scope's containing frame, which is the parallel terminal.
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

**Other workers' exceptions are lost.** Only the trigger surfaces.
This is intentional — gathering every worker's outcome is what § 3.2
("keep the error as data") is for.

`CancellationException` is a **local concern**. It does not escape
the scope. If a sibling has a `try { ... } catch (CancellationException
e) { ... }` around its own work, that catch sees it (and can use
`e.getCause()` to inspect the trigger); otherwise the cancellation
unwinds the sibling silently, drops fire, the sibling's slot finishes.

### §2.3 Drop chain on the throw path

`ErrorModel.md § 201` covers this for general code; the parallel-
specific notes:

- Each worker fiber has its own drop chain (per-fiber linked list).
- On a worker throw, the worker's chain unwinds top-down to the spawn
  frame.
- The fiber trampoline runs the spawn-frame drop entries (the
  formal-parameter slots receive their owned values; those drop here)
  before storing the exception in the Task slot.
- Sibling workers cancelled by scope unwind their own chains in the
  same way.
- The orchestrator's drop chain is unaffected until the scope's
  closing brace; on re-raise of the trigger, the orchestrator's
  locals drop as part of the terminal's own unwind.

The result: an exception in parallel is **no leakier** than an
exception in sequential. Every fiber cleans up after itself.

### §2.4 Worker-side cancellation cooperative-checkpoint

A cancelled sibling sees the cancel flag at its next pull-loop
iteration (the `while (o.isPresent())` guard checks the scope's
flag implicitly via `next()` returning the cancellation signal, or
the worker explicitly polls — see `Thread.md § Cooperative
cancellation`). Workers that don't have a checkpoint loop (rare for
streams — every stream worker pulls from `next()`) won't observe
the cancel until they next reach one.

The single-carrier scheduler today serializes worker execution, so
the "cancel observation latency" is the work between checkpoints
of a single fiber, not parallel-runtime latency. Multi-carrier
will need TLS-promoted `__cajeta_exc_top` and per-fiber cancel-flag
hot-paths (`ErrorModel.md § TLS hardening`); the user-visible
contract above doesn't change.

---

## §3 User patterns for tolerance

The locked default is **fail-fast**. The patterns below let the user
choose tolerance at the layer where it makes sense — the lambda, the
element shape, or the stdlib helper. The terminal interface stays
unchanged.

### §3.1 Pattern A — lambda-local try/catch

The simplest answer to "don't let one exception kill the run":

```cajeta
int32 sum = xs.stream()
              .parallel()
              .map(x -> {
                  try { return riskyTransform(x); }
                  catch (IOException e) { return 0; }   // sentinel
              })
              .reduce(0, (a, b) -> a + b, (a, b) -> a + b);
```

The worker never throws. The scope sees no exception. No
cancellation. The sum is computed over (successful elements +
sentinels). Use this when:
  - You can pick a sentinel that's harmless under your accumulator
    (0 for sum, identity for product, "" for string-concat).
  - You don't need to know which elements failed.

### §3.2 Pattern B — keep the error as element data

Carry both outcomes through the pipeline as the element type:

```cajeta
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

Stream<Outcome<Score>> results = urls.stream()
    .parallel()
    .map(u -> Outcome.tryRun(() -> fetchScore(u)));

// downstream: separate ok from err. Today this needs .sequential()
// before collect because parallel collect is blocked on the
// Collector supplier gap (see StreamParallelism.Examples.md § 7.9).
results = results.sequential();
ArrayList<Score> ok = results.filter(o -> o.ok).map(o -> o.value).collect(Collectors.toList());
ArrayList<Exception> errs = results.filter(o -> !o.ok).map(o -> o.error).collect(Collectors.toList());
```

`Outcome<T>` is **user-defined**, not stdlib. The stream layer
doesn't impose it; it imposes nothing about element types other
than `T`. This is what we mean by "we do not wrap into Try/Result."

Use this when:
  - You want every failure to surface, not just the first.
  - The downstream wants both successes and errors.
  - The element count is bounded enough that materializing the full
    set is reasonable.

For "I only want the count of failures" or "I want to know the
total but tolerate misses," `Outcome<T>` is heavier than a sentinel
+ counter (§ 3.4). Pick the right tool.

### §3.3 Pattern C — opt-in stdlib helpers

When the lambda-local try/catch is verbose enough to obscure intent,
the stdlib supplies named operators that codify a recovery shape:

```cajeta
public final #Stream<R> mapOrSkip<R>((T) -> R fn);
public final #Stream<R> mapOrFallback<R>((T) -> R fn, R fallback);
public final #Stream<R> mapOrLog<R>((T) -> R fn,
                                    (T, Exception) -> void logger);
```

Behavior:
- **mapOrSkip** — elements whose `fn` throws `RecoverableException`
  are DROPPED. Stream shortens.
- **mapOrFallback** — elements whose `fn` throws `RecoverableException`
  are SUBSTITUTED with the user-supplied `fallback`. Stream length
  unchanged.
- **mapOrLog** — elements whose `fn` throws `RecoverableException`
  are passed to `logger(elem, exception)` and then DROPPED.

`UnrecoverableException` (assertion failure, OOM, etc.) is NEVER
caught by these helpers — the alarm contract from `ErrorModel.md §
The hierarchy` is preserved. The user can still catch it at the
terminal or let the runtime's default catch handle it.

These are not magic. They expand to the same lambda-local try/catch
pattern; the operator name is the documentation that "failures here
are being suppressed by design." A reader doesn't have to scan a
trailing `.onError(...)` decorator to know.

```cajeta
int32 sum = urls.stream()
                .mapOrFallback(u -> fetchScore(u), 0)
                .reduce(0, (a, b) -> a + b);
```

These helpers catch inside the lambda body, so the parallel driver
sees no worker exceptions and no cancellation — parallel-clean.

**Type-changing wrapper caveat.** Like `Stream<T>.map<R>`, these
helpers are type-changing wrappers (`Stream<T> -> Stream<R>`). Under
`.parallel()` the wrapper-chain walk stops at the type-flip, so the
chain falls back to sequential — same documented limitation as the
other type-changing intermediates (`StreamParallelism.Examples.md §
7.9`). Tests for this surface live in `StreamIntermediateTests.cpp`
as `mapOr{Skip,Fallback,Log}*`.

### §3.4 Pattern D — threshold-based abort

"Tolerate up to N failures, then fail the whole run." A shared
counter inside the lambda:

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
locked default: that worker's exception becomes the trigger,
siblings cancel, the terminal re-raises, the caller's `catch`
handles the abort.

**Single-carrier note.** The `failCount[0] = failCount[0] + 1` is
not atomic. On the single-carrier scheduler today, workers
serialize, so the increment is observably correct. When multi-
carrier lands, the counter wants `AtomicInt32` (Thread.md §
Atomics); the pattern doesn't change, only the type.

Use this when:
  - You want a circuit-breaker, not just suppression.
  - The threshold is small (close behavior dominates the spec, not
    every "tolerate the noise" case).

---

## §4 Combiner / accumulator exceptions (parallel terminals)

The combiner of a parallel reduce / fold runs on the
**orchestrator** after the scope joins. If the combiner throws:

  - Worker tasks have already completed (some successfully, others
    cancelled-then-completed).
  - The orchestrator's local exception unwinds normally — the
    partial values still live in `partials[]` and drop as part of
    the orchestrator's unwind.
  - The caller sees the combiner's exception, not a worker
    exception.

This is **not** AggregateException territory. If a worker had
thrown earlier in the same call, the scope's join-pass would have
re-raised the trigger before the combiner ran — the combiner
wouldn't be reached. The combiner-throws case is post-success only.

```cajeta
try {
    R result = xs.stream()
                 .parallel()
                 .fold(seed, (acc, x) -> riskyAcc(acc, x), (a, b) -> riskyCombine(a, b));
} catch (Exception e) {
    // Could be from any worker's riskyAcc OR the orchestrator's
    // riskyCombine. The user catches at the terminal regardless.
}
```

---

## §5 What we explicitly do NOT do

(Re-stating ErrorModel.md § Streams' negatives in parallel context.)

  - **No decorator chains.** No `.onErrorReturn(...)`,
    `.onErrorResume(...)`, `.retry(n)`. They live in the wrong
    place — separated from the producing operation that failed —
    and flatten per-step context.
  - **No stream-level Try/Result wrapping.** The pipeline doesn't
    mandate `Try<T>` or `Result<T, E>`. Users who want that shape
    define their own `Outcome<T>` (§ 3.2); the pipeline is
    type-agnostic.
  - **No catch-by-step syntax.** `.map(λ).catch(IOException, λ).filter(λ)`
    treats the pipeline as a try/catch region. It isn't.
  - **No `AggregateException` at the terminal.** First-throw wins
    in the scope's join pass. Other workers' exceptions are lost.
    Future opt-in `scope.collectAll() -> List<Result<T>>` may give
    a per-task outcome list at the *scope* layer (not the stream
    layer) — deferred per `ErrorModel.md § 197`.
  - **No `.parallel(maxFailures=N)` knob.** Threshold-based abort
    lives in user-authored lambdas (§ 3.4), not the terminal
    surface. The reason is the same as why we reject the decorator
    chain: lambda-local code knows what "failure" means; the
    terminal doesn't.
  - **No checked-exception cascade.** `throws` clauses on lambdas
    are documentation, not enforcement (per the language's overall
    error position).

---

## §6 Cross-references

  - `ErrorModel.md § The hierarchy` — RecoverableException vs
    UnrecoverableException; what `catch (Exception e)` catches.
  - `ErrorModel.md § Async / fiber integration` — Task<T>.exception
    slot, fiber trampoline, scope escalation.
  - `ErrorModel.md § Streams (forward-looking)` — the original
    locus of the stream policy. The detail here supersedes it for
    parallel behavior; the position statements there still hold.
  - `StreamParallelism.md § Goals (item 4)` — the cooperative
    cancellation contract.
  - `StreamParallelism.md § Locked decisions` — findFirst →
    findAny under parallel; the fail-fast default; combiner-required.
  - `StreamParallelism.Examples.md § 7.9` — errata items that
    interact with error handling (collect needs supplier, etc.).
  - `Thread.md § Cooperative cancellation`, `Thread.md § Atomics` —
    underlying machinery that the patterns here rely on.

---

## §7 Summary table

| Goal | Pattern | Mechanism |
| ---- | ------- | --------- |
| Suppress all failures, use sentinel | § 3.1 lambda try/catch | catch in lambda body, return sentinel |
| Suppress, named operator | § 3.3 stdlib helper | mapOrSkip / mapOrLog / mapOrFallback |
| Keep both outcomes, partition downstream | § 3.2 Outcome<T> | user-defined element type |
| Abort after N failures | § 3.4 threshold | shared counter + lambda throw |
| Default: stop on first failure | § 2.2 fail-fast | scope's first-throw-wins (locked) |
