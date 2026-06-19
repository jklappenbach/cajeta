# Cajeta Error Model — Specification v1

## Goals

- **Familiar syntax.** Java/C++-style `try` / `catch` / `throw`. No new operators, no `T!E` value types, no `?` propagation sugar. The mental model carries over from every mainstream OO language.
- **Two tiers, expressed through the type hierarchy.** `UnrecoverableException` is the alarm — the program (or task) shouldn't continue. `RecoverableException` is normal failure that the caller may want to handle.
- **Signatures document, the compiler doesn't enforce.** A method's `throws` clause lists the `RecoverableException` subtypes that can flow out. The compiler emits a **warning** (not an error) when a call site doesn't acknowledge — never the Java-style enforced cascade that drives teams to wrap everything as `RuntimeException`.
- **Default catch at every entry point.** Both `main()` and each spawned fiber are wrapped in a runtime-level catch. Recoverable exceptions are logged + the entry returns; unrecoverable exceptions log and abort the process. Programs don't crash silently.
- **Composes with what exists.** Drop chain unwinds owned locals on the throw path the same way it does on the return path. Async tasks carry an exception slot; `await` re-raises into the awaiter's frame.

## Non-goals (v1)

- Compiler-enforced `throws` cascades (Java's checked-exception failure mode).
- Resumable exceptions / effect handlers.
- `Result<T, E>`-style value-typed errors with a propagation operator. Reserved as a future opt-in stdlib pattern for hot paths if measurements demand it; not part of v1's surface.
- Multiple-error-type unions in a signature beyond what subtyping already provides.
- Stack trace capture on every `Recoverable` throw (cost — see Stack traces below).

## The hierarchy

All four roots live in `package cajeta.error;` (the implicitly-loaded stdlib prelude registers them there). User-defined exceptions should extend one of these types; the package convention keeps exception hierarchies discoverable and encourages reuse over re-rolling.

```
Throwable                           (root, carries `message`)
└── Exception                       (adds `cause` for chain-of-causality)
    ├── UnrecoverableException      (the alarm — terminates the process)
    │   ├── AssertionError          (illustrative — not yet declared)
    │   ├── OutOfMemoryError        (illustrative — not yet declared)
    │   └── ...
    └── RecoverableException        (caller may handle)
        ├── IOException             (illustrative — not yet declared)
        │   ├── FileNotFoundException
        │   └── TimeoutException
        ├── ParseException
        └── ...
```

> **What actually ships today.** Only the four roots — `Throwable`,
> `Exception`, `RecoverableException`, `UnrecoverableException` — are declared
> in `cajeta.error`. The leaf types above (`AssertionError`, `IOException`,
> `FileNotFoundException`, `TimeoutException`, `ParseException`, …) are
> *illustrative* of how user and stdlib code extends the hierarchy; they are
> not yet in the prelude (tracked in *Known gaps* below). Domain exceptions
> that **do** exist live in their owning packages — e.g. `cajeta.io.file.IoException`,
> `cajeta.io.net.*` (`ConnectionRefusedException`, `TimedOutException`, …),
> `cajeta.codec.Base64Exception`, `cajeta.time.DateTimeException` — and each
> extends one of the four roots.

The split between `Throwable` and `Exception` is deliberate: `Throwable` is the root identity for "anything throwable" (catch-all type), while `Exception` is where the cause chain lives. Any `Exception` (and therefore any Recoverable or Unrecoverable) can record what caused it; bare `Throwable`s without a cause field are theoretically possible but in practice every thrown thing goes through `Exception` or a subclass.

- `Throwable` is the common root — anything that can be thrown carries a `message`. `Exception` adds `cause` (a typed `Throwable` pointing at the underlying exception when this throw is itself the result of catching-and-rewrapping a lower-level failure). Walking the cause chain at print time gives the full "X was caused by Y was caused by Z" stack visibility every layer's catch site contributed.
- `UnrecoverableException` is for conditions the program has no plan for: assertion failures, exhausted memory, contract violations, unreachable branches. Throwing one terminates the process (after the drop chain unwinds).
- `RecoverableException` is for failures the caller is expected to deal with: I/O errors, parse failures, timeouts, business-rule violations.
- User-defined exceptions extend one or the other. The choice is a design decision the author makes when defining the exception type.

## Signatures

A method that can throw a `RecoverableException` subtype declares it in its `throws` clause:

```cajeta
public int32 readPort(int32 port) throws IOException {
    if (port < 0) {
        throw heap IOException("invalid port: " + port);
    }
    return 42;
}
```

`UnrecoverableException` subtypes do **not** appear in `throws` clauses — any method can throw them. They're not part of the contract; they're the alarm.

Multiple recoverable types are comma-separated:

```cajeta
public Response fetch(Url u) throws IOException, TimeoutException, AuthException {
    ...
}
```

A method declared `throws RecoverableException` (the root) can throw any recoverable type — useful for orchestrators that wrap many error sources.

## Call-site behavior

A call to a `throws`-declaring method does **not** require a surrounding `try`. The compiler emits a **warning** when no enclosing handler catches the declared exception type:

```cajeta
public Response getOrLog(Url u) {
    Response r = fetch(u);   // warning: uncaught throws IOException, TimeoutException, AuthException
    return r;
}
```

The warning is suppressible with the lint annotation, using the rule ID `uncaught-throws`:

```cajeta
@SuppressLint("uncaught-throws")
public Response getOrLog(Url u) {
    return fetch(u);  // silent: caller accepts that these propagate
}
```

Why warning rather than error: Java's hard requirement to handle or declare every checked exception is the failure mode the community revolted against. The throws clause's value is **documentation**; making it a soft signal preserves that value without the refactor cascade. Wrapping every fallible call in `try { ... } catch { ... }` was never the point.

## try / catch / finally

Catch by type. Multiple `catch` arms dispatch on the most-specific matching type:

```cajeta
public int32 robustRead() {
    try {
        return readPort(8080);
    } catch (FileNotFoundException e) {
        log.warn("no such port: " + e.message);
        return -1;
    } catch (IOException e) {
        log.error("io: " + e.message);
        return -2;
    } catch (TimeoutException e) {
        return readPort(8080);  // one retry
    }
}
```

`finally` is supported. The grammar accepts `try block (catchClause+ finallyBlock? | finallyBlock)` — i.e. a `try` with catch arms, an optional trailing `finally`, or a bare `try`/`finally` with no catch. The `finally` block runs on **every** exit edge from the `try`/`catch` region: normal fall-through, an explicit `return`/`break`/`continue` escaping the body, and exception unwind — including a throw out of a `catch` arm, which runs the `finally` before propagating.

```cajeta
try {
    return readPort(8080);
} finally {
    log.info("readPort attempt finished");   // runs on return AND on throw
}
```

That said, prefer owned locals for resource cleanup where you can: an owned local's drop fires on every exit path (return, throw, fall-through) via the drop chain, with no explicit `finally` needed. Reach for `finally` when the cleanup isn't tied to a single owned value (logging, flag reset, releasing a non-owning handle).

A bare `catch` (no type) catches everything:

```cajeta
try {
    riskyOp();
} catch (e) {
    log.error("something failed: " + e);
}
```

Catches by-type are dispatched in source order; the first arm whose type is a supertype (or exact match) of the thrown exception wins. Same rules as Java.

## Throwing

```cajeta
throw heap IOException("disk full");
```

`throw` is a statement (like `return`). The exception value must be an instance of `Throwable` (or subtype). The drop chain unwinds owned locals on the way up.

## Unrecoverable exceptions and the system default catch

Unrecoverable exceptions are the alarm. The runtime wraps `main()` (and each spawned fiber's entry trampoline) in a default catch:

- If `main()` exits via an uncaught `RecoverableException`: the runtime emits `cajeta: uncaught exception: <message>` to stderr along with the captured stack trace, then exits with code 1 (clean exit).
- If `main()` exits via an uncaught `UnrecoverableException`: the runtime emits `cajeta: unrecoverable exception: <message>` + stack trace to stderr, then `abort()`s — generates SIGABRT for core-dump / debugger inspection. The "uncaught alarm" is itself a fatal condition.
- If a fiber's body throws `UnrecoverableException`: log + `abort()` the entire process. Per-fiber recovery from unrecoverables isn't safe — the runtime invariant has been violated, and propagating it through `await` would let the corruption hide.
- If a fiber's body throws `RecoverableException`: store the exception on the `Task<T>`'s exception slot, signal done. The awaiter re-raises into its own frame when it does `await task`.

Implementation: every class vtable carries a `parent_vtable` pointer at a fixed offset (`CAJETA_VTABLE_PARENT_OFFSET`, NULL at the root). Codegen (`Compiler::emitUnrecoverableMarker`) emits a module global constructor that calls the runtime setter `__cajeta_set_unrecoverable_vtable(UnrecoverableException's vtable)` once at startup, stashing it in the runtime's `g_unrecoverable_vtable` static. The runtime helper `__cajeta_is_unrecoverable(void* throwable)` reads the instance's vtable (slot 0) and walks the `parent_vtable` chain upward, matching against `g_unrecoverable_vtable` — returns 1 for any descendant of `UnrecoverableException`, 0 otherwise (and 0 defensively for legacy bare-integer throws, whose "pointer" falls below the zero-page boundary). The global-ctor + plain-call scheme resolves identically across ELF/MachO/COFF and in both JIT and AOT.

User code can absolutely `catch (UnrecoverableException e)` if it really wants to — the language doesn't forbid it. The convention is "don't, unless you have an extremely good reason" (a top-level supervisor in a long-running daemon may legitimately want to log-and-keep-going for some kinds of unrecoverable). The system catch is the safety net for everyone who doesn't.

## Stack traces

Stack-trace capture is a **single compiler-wide flag**, `--stack-trace-capture=on|off`, **default `on`**. When enabled, every throw site walks the native call stack via `backtrace(3)` (glibc/macOS) and records the frames on the thrown instance, regardless of whether it's recoverable or unrecoverable. There is no per-exception or per-construction opt-in/opt-out; the flag is read once and applies to all throws (the runtime mirrors it through `__cajeta_set_stack_trace_capture` / `__cajeta_get_stack_trace_capture`).

```
cajeta --stack-trace-capture=off  program.cajeta   # disable capture (hot-loop throws)
```

Two practical caveats from the implementation:

- **Capture is skipped inside a fiber.** `backtrace(3)` walks the OS-thread stack; a parked/resumed fiber runs on a swapped `ucontext` stack where the native walk isn't meaningful, so throws raised on a fiber don't carry a native trace today.
- The debug build presets turn capture **off** by default (it's `true` only in the default flag set), since an attached debugger supplies the stack itself.

If no trace was captured (flag off, or thrown on a fiber), the printed exception shows type + message + cause chain only.

## Async / fiber integration

### Task<T> carries an exception slot

`CajetaTask`'s layout extends from `{ T value, i32 done }` to `{ T value, i32 done, Throwable* exception }`. The trampoline that runs a fiber wraps the inner call in a try:

- Normal completion → store value, set done.
- `RecoverableException` thrown by the body → store the exception, set done. (Don't propagate to the carrier OS thread.)
- `UnrecoverableException` thrown by the body → log + abort the process.

`await` checks the exception slot on resume; if non-null, re-raises into the awaiter's frame.

```cajeta
async int32 fetchScore(Url u) throws IOException {
    Response r = http.get(u);  // can throw IOException
    return r.statusCode;
}

public int32 caller() throws IOException {
    return await spawn fetchScore(u);  // IOException re-raised here at await
}
```

The `throws` clause on the async method documents what the awaiter might see; the warning behavior at call sites still applies.

### scope and exception escalation (R5-D)

When a scope joins child tasks:

1. Walk every registered child; await each (`__cajeta_scope_exit` / `__cajeta_scope_exit_to`).
2. The moment a child's exception slot is non-null, record it as the **trigger**.
3. Cancel every remaining still-running sibling: `__cajeta_fiber_cancel(fiber, trigger)` stores the **trigger itself** in each sibling's `cancel_with` marker. At the sibling's next `await` park-resume, `__cajeta_task_wait` re-raises that marker.
4. Keep waiting on the rest so the scope still joins everything before unwinding.
5. Re-raise the **trigger's** original exception to the scope's containing frame — unwrapped, no wrapper.

There is **no `CancellationException` type**. The fiber model removes the thread-boundary that justified wrapping in older designs (Java's `ExecutionException`, etc.) — the trigger flows through await/scope the same way a thrown exception flows through any function call, and a cancelled sibling re-raises *that same trigger Throwable*, not a synthetic cancellation marker. Callers handle `catch (IOException e)` directly. (If a sibling needs to know what triggered its cancel, the thrown trigger *is* that information; the `cause` field on `Exception` is available for chaining if a handler rewraps.)

**No `AggregateException`.** Multiple-children-failing-simultaneously is rare and the caller usually wants one error to handle. If a use case ever demands collecting every child's outcome, a stdlib `scope.collectAll() -> List<Result<T>>` helper can be added without changing core semantics — it would build on top of the per-task exception slot the trampoline already populates.

**TLS hardening (shipped).** The setjmp/longjmp exception chain head (`__cajeta_exc_top`) and the drop-chain head were promoted off their single global slots: `cajeta_fiber` now carries per-fiber `exc_top` / `drop_top` fields, with `__thread`-qualified `__cajeta_main_exc_top` / `__cajeta_main_drop_top` for the main OS thread, selected through `__cajeta_exc_top_ptr()` / `__cajeta_drop_top_ptr()` (the same pattern `scope_top` uses). This was necessary even under today's single-carrier cooperative model, because the carrier runs on a separate OS thread from `main`, so the two could race on a shared global. See `AsyncStatus.md`.

### Drop chain on the throw path

Owned locals drop on the throw path the same as on the return path. The existing drop-chain machinery (with the watermark) handles both; no exception-specific bookkeeping needed.

## Streams (forward-looking)

Cajeta does not yet have a stream API; this section pins the policy for when one lands so the door doesn't get left open to drift toward a decorator model.

**Detailed spec.** See `StreamParallelism.ErrorHandling.md` for the parallel-stream walk-through (worker-throw → scope join → trigger re-raise → drop chain), the user-side tolerance patterns (lambda try/catch, user-authored Outcome<T>, mapOr* helpers, threshold abort), and the explicit non-goals re-stated in parallel context. The position below is the policy; the companion doc is the specification.

The core position: **stream operations don't have their own error-handling sub-DSL.** Lambdas inside `.map(...)`, `.filter(...)`, `.flatMap(...)` can throw like any other code; the exception propagates through the pipeline; the caller wraps the **terminal operation** in `try/catch` if recovery is wanted.

```cajeta
try {
    List<int32> codes = urls.stream()
        .map(u -> http.get(u).statusCode)   // can throw IOException
        .filter(c -> c >= 200 && c < 300)
        .toList();
} catch (IOException e) {
    log.warn("fetch failed: " + e.message);
}
```

Per-element recovery is per-element try/catch *inside the lambda*:

```cajeta
urls.stream()
    .map(u -> {
        try { return http.get(u).statusCode; }
        catch (IOException e) { return -1; }
    })
    .toList();
```

Verbose for the per-element case, but explicit: a reader sees what's caught and where.

**Convenience helpers (stdlib, not core).** Where the verbose pattern is too common to write out, the stdlib can supply named operators that codify a specific recovery shape:

- `mapOrSkip(λ)` — drop elements whose lambda threw `RecoverableException`. Returns a shorter stream.
- `mapOrLog(λ, logger)` — log and drop.
- `mapOrFallback(λ, fallback)` — replace failed elements with a default.

These are opt-in by name. The pipeline's intent stays readable: a reader doesn't have to scan a trailing `.onError(...)` decorator to discover that failures are being suppressed.

**Parallel streams.** Mirror scope's join semantics: first-throw wins, sibling workers get cancelled, the trigger exception is re-raised at the terminal operation. No `AggregateException` at the stream layer either.

**Explicitly NOT in the design:**

- **Decorator chains** (RxJava-style `.onErrorReturn(...)` / `.onErrorResume(...)`). They live in the wrong place — separated from the producing operation that failed — and flatten per-step context. Avoid.
- **Stream-element `Try<T>` / `Result<T, E>` wrapping** (Vavr-style). Conflicts with our exception model; mixing in streams confuses users about which world they're in.
- **Stream-specific catch-by-step syntax** (`.map(λ).catch(IOException, λ).filter(λ)`). Treats the pipeline as a try/catch — but a pipeline isn't a region of code, it's a sequence of transformations applied to elements.

## Examples

### Example 1: catch and recover

```cajeta
public Config loadConfig(String path) {
    try {
        return Config.parse(File.readAll(path));   // throws IOException, ParseException
    } catch (FileNotFoundException e) {
        return Config.defaultsFor(path);            // file's optional
    } catch (ParseException e) {
        log.error("config malformed: " + e.message);
        return Config.empty();
    }
    // IOException (other than FileNotFound) is uncaught — warning at compile,
    // system catch at main if it propagates that far.
}
```

### Example 2: declare and propagate

```cajeta
public Response fetchOrAuth(Url u) throws IOException, AuthException {
    Response r = http.get(u);   // throws IOException, TimeoutException
    if (r.statusCode == 401) {
        Token t = refreshAuth();   // throws AuthException
        r = http.get(u, t);
    }
    return r;
    // TimeoutException is uncaught but undeclared — compiler warning.
    // Suppress with @SuppressLint("uncaught-throws"), or add it to the throws clause,
    // or wrap with try/catch.
}
```

### Example 3: unrecoverable (alarm)

```cajeta
public int32 vtableLookup(VTable v, int64 hash) {
    int32 lo = 0;
    int32 hi = v.entries.size() - 1;
    while (lo <= hi) {
        int32 mid = (lo + hi) / 2;
        int64 mhash = v.entries[mid].hash;
        if (mhash == hash) return mid;
        if (mhash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    throw heap AssertionError("vtable hash not found — caller invoked a method not in this vtable");
}
```

`AssertionError extends UnrecoverableException`. The caller doesn't catch; if hit, the process aborts with a stack trace.

### Example 4: async + scope

```cajeta
public async int32 aggregateScores(List<Url> urls) throws IOException {
    int32 total = 0;
    scope {
        for (Url u : urls) {
            total = total + await spawn fetchScore(u);   // fetchScore throws IOException
        }
    }
    return total;
}
```

If any spawned `fetchScore` throws, the `await` re-raises into the surrounding frame. Control leaves the loop; `scope` cancels the remaining children, waits for their unwinds, then propagates the exception up to the caller of `aggregateScores`. `total` drops on the throw path the same as on a normal return.

### Example 5: suppressing the warning

```cajeta
@SuppressLint("uncaught-throws")
public void crashOnPurpose() {
    riskyOp();   // declared throws WeirdException; we want it to propagate
}
```

Used when a method is intentionally a thin pass-through and the documenting-throws-clause noise isn't worth it.

### Example 6: per-task isolation in a daemon

```cajeta
public void daemonLoop() {
    while (true) {
        Request r = queue.take();
        try {
            handle(r);   // recoverables don't kill the daemon
        } catch (RecoverableException e) {
            log.warn("request failed: " + e.message);
            // continue to next request
        }
        // Unrecoverable propagates → system catch → abort. Daemon stops.
        // That's correct: an Unrecoverable is the alarm.
    }
}
```

## Rationale (why not the alternatives)

- **C++-style unchecked exceptions** — no signature documentation, silent ABI contracts, teams either ban exceptions entirely or wrap everything. We keep the documentation value without the enforcement penalty.
- **Java's enforced checked exceptions** — the `throws` cascade is the well-documented failure mode. Refactors force noise into every caller; lambdas can't carry checked exceptions cleanly; teams wrap everything as `RuntimeException` to escape. We retain Java's documentation idea but drop the enforcement.
- **C#/Java unchecked-only** — no compile-time signal at all. Readers learn what a method throws by reading the source or testing it. We add the throws clause back as documentation.
- **Rust `Result<T, E>` + `?`** — value-typed errors are clean but force a different control-flow paradigm. They also have real cost (every call site has the `?` branch, every fallible function's result has the sum tag at the ABI level). For Cajeta, the syntactic familiarity of try/catch and the zero happy-path cost of the existing setjmp/longjmp infrastructure are stronger arguments. (We may add a stdlib `Result<T, E>` later for hot paths where exception unwinding is too expensive.)
- **Zig `E!T`** — same critique as Rust, plus the operator-order debate we already went through.
- **Swift `throws` (untyped) + `try`** — closer to what we want, but Swift's choice to type-erase the error sacrifices the documentation value. We keep the type list.
- **Effect systems** — academic; not worth the cognitive cost in v1.

## Deferred / out of scope (v1)

- **`Result<T, E>` stdlib pattern.** Optional value-typed error returns for hot paths where exception unwind cost matters. Just a sealed sum + helpers, no compiler magic. Add when measurements demand.
- **`@Throws(infer)`** — compiler-inferred throws lists. Possible future ergonomic; today the dev writes the list.
- **`when` guards in catch arms** (`catch (IOException e) when (e.code == EBUSY)`). Add if patterns demand it.
- **Type-scoped suppression** — suppress the `uncaught-throws` warning for only specific exception types, rather than the whole method. Today `@SuppressLint("uncaught-throws")` is all-or-nothing for the annotated declaration.

## Known gaps (v1 spec → implementation)

This document is the spec. The v1 surface has shipped; status against the original punch list:

- [x] Stdlib: declare the four roots `Throwable` / `Exception` / `RecoverableException` / `UnrecoverableException` in `cajeta.error`. **Built-in leaf subtypes** (`AssertionError`, `OutOfMemoryError`, a unified `IOException`/`TimeoutException`/`CancellationException` set) are **not yet declared** — domain exceptions live in their owning packages instead (`cajeta.io.file.IoException`, `cajeta.io.net.*`, …). A common prelude set remains open.
- [x] Parser: `throws` clause on method declarations; stored on `Method` (`throwsList`, advisory only).
- [x] Type-checker / lint: the `uncaught-throws` lint walks the called method's throws clause and an enclosing-try stack, suppressing the warning when a catch arm covers the type (supertype-aware) or it's declared/suppressed. Suppression is `@SuppressLint("id")` (generalized from `@SuppressUncaughtThrow`).
- [x] Runtime: exception path carries a `Throwable*` over a `void*` carrier (migrated off the bare `int64`).
- [x] Codegen: stack-trace capture via the global `--stack-trace-capture` flag (default on, all throws) — *not* the per-kind opt-in/opt-out originally sketched here.
- [x] System default catch: wraps `main()` and each fiber trampoline; recoverable vs unrecoverable distinguished by the `__cajeta_is_unrecoverable` vtable walk.
- [x] Async integration: `CajetaTask` carries an exception slot; the trampoline stores a recoverable throw in it and `await` re-raises into the awaiter; an unrecoverable aborts.
- [x] R5-C (cancellation): per-fiber `cancel_with` marker (the trigger Throwable), checked on await resume. **No** `CancellationException` type — the trigger itself is re-raised.
- [x] R5-D (scope exception escalation): `scope_exit` inspects each child's exception slot; first thrower wins; siblings are cancelled with the trigger; the trigger re-raises into the containing frame.
