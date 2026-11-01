# Cajeta Error Model — Specification v1

## Goals

- **Familiar syntax.** The language uses Java and C++ style `try` / `catch` / `throw`. There are no new operators, no `T!E` value types and no `?` propagation sugar. The mental model carries over from every mainstream OO language.
- **Two tiers, expressed through the type hierarchy.** `UnrecoverableException` is the alarm, and it means the program or task should not continue. `RecoverableException` is normal failure that the caller may want to handle.
- **Signatures document, the compiler doesn't enforce.** A method's `throws` clause lists the `RecoverableException` subtypes that can flow out. The compiler emits a warning when a call site does not acknowledge them. It is a warning and never an error, so there is no Java-style enforced cascade of the kind that drives teams to wrap everything as `RuntimeException`.
- **Default catch at every entry point.** Both `main()` and each spawned fiber are wrapped in a runtime-level catch. A recoverable exception is logged and the entry returns. An unrecoverable exception is logged and the process aborts. Programs never crash silently.
- **Composes with what exists.** The drop chain unwinds owned locals on the throw path the same way it does on the return path. Async tasks carry an exception slot, and `await` re-raises into the awaiter's frame.

## Non-goals (v1)

- Compiler-enforced `throws` cascades (Java's checked-exception failure mode).
- Resumable exceptions / effect handlers.
- `Result<T, E>`-style value-typed errors with a propagation operator. They are reserved as a future opt-in stdlib pattern for hot paths if measurements demand it, and they are not part of v1's surface.
- Multiple-error-type unions in a signature beyond what subtyping already provides.
- Stack trace capture on every `Recoverable` throw, because of its cost. See Stack traces below.

## The hierarchy

All four roots live in `package cajeta.error;`, where the implicitly loaded stdlib prelude registers them. User-defined exceptions should extend one of these types. The package convention keeps exception hierarchies discoverable and encourages reuse over re-rolling.

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

> **What actually ships today.** Only the four roots, `Throwable`,
> `Exception`, `RecoverableException` and `UnrecoverableException`, are declared
> in `cajeta.error`. The leaf types above (`AssertionError`, `IOException`,
> `FileNotFoundException`, `TimeoutException`, `ParseException`, …) illustrate
> how user and stdlib code extends the hierarchy. They are not yet in the
> prelude, and *Known gaps* below tracks them. The domain exceptions that do
> exist live in their owning packages, for example `cajeta.io.file.IoException`,
> `cajeta.io.net.*` (`ConnectionRefusedException`, `TimedOutException`, …),
> `cajeta.codec.Base64Exception` and `cajeta.time.DateTimeException`, and each
> extends one of the four roots.

The split between `Throwable` and `Exception` is deliberate. `Throwable` is the root identity for anything throwable and serves as the catch-all type, while `Exception` is where the cause chain lives. Any `Exception`, and therefore any Recoverable or Unrecoverable, can record what caused it. A bare `Throwable` without a cause field is theoretically possible, but in practice every thrown thing goes through `Exception` or a subclass.

- `Throwable` is the common root, so anything that can be thrown carries a `message`. `Exception` adds `cause`, a typed `Throwable` that points at the underlying exception when this throw is itself the result of catching and rewrapping a lower-level failure. Walking the cause chain at print time gives the full "X was caused by Y was caused by Z" visibility that every layer's catch site contributed.
- `UnrecoverableException` is for conditions the program has no plan for, such as assertion failures, exhausted memory, contract violations and unreachable branches. Throwing one terminates the process after the drop chain unwinds.
- `RecoverableException` is for failures the caller is expected to deal with, such as I/O errors, parse failures, timeouts and business-rule violations.
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

`UnrecoverableException` subtypes do not appear in `throws` clauses, because any method can throw them. They are the alarm, and they are outside the contract.

Multiple recoverable types are comma-separated:

```cajeta
public Response fetch(Url u) throws IOException, TimeoutException, AuthException {
    ...
}
```

A method declared `throws RecoverableException`, the root, can throw any recoverable type. This is useful for orchestrators that wrap many error sources.

## Call-site behavior

A call to a `throws`-declaring method does not require a surrounding `try`. The compiler emits a warning when no enclosing handler catches the declared exception type:

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

It is a warning and never an error, because Java's hard requirement to handle or declare every checked exception is the failure mode the community revolted against. The value of the throws clause is documentation. Making it a soft signal preserves that value without the refactor cascade, and you are never pushed to wrap every fallible call in `try { ... } catch { ... }`.

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

`finally` is supported. The grammar accepts `try block (catchClause+ finallyBlock? | finallyBlock)`, which means a `try` with catch arms, an optional trailing `finally`, or a bare `try`/`finally` with no catch. The `finally` block runs on every exit edge from the `try`/`catch` region. Those edges are normal fall-through, an explicit `return`/`break`/`continue` escaping the body, and exception unwind. A throw out of a `catch` arm also runs the `finally` before propagating.

```cajeta
try {
    return readPort(8080);
} finally {
    log.info("readPort attempt finished");   // runs on return AND on throw
}
```

Prefer owned locals for resource cleanup where you can. An owned local's drop fires through the drop chain on every exit path, whether return, throw or fall-through, so no explicit `finally` is needed. Reach for `finally` when the cleanup is not tied to a single owned value, such as logging, a flag reset or releasing a non-owning handle.

A bare `catch` with no type catches everything:

```cajeta
try {
    riskyOp();
} catch (e) {
    log.error("something failed: " + e);
}
```

Catches by type are dispatched in source order, and the first arm whose type is a supertype or exact match of the thrown exception wins. The rules are the same as Java's.

## Throwing

```cajeta
throw heap IOException("disk full");
```

`throw` is a statement, like `return`. The exception value must be an instance of `Throwable` or a subtype. The drop chain unwinds owned locals on the way up.

## Unrecoverable exceptions and the system default catch

Unrecoverable exceptions are the alarm. The runtime wraps `main()` and each spawned fiber's entry trampoline in a default catch:

- If `main()` exits via an uncaught `RecoverableException`, the runtime emits `cajeta: uncaught exception: <message>` to stderr along with the captured stack trace, then exits cleanly with code 1.
- If `main()` exits via an uncaught `UnrecoverableException`, the runtime emits `cajeta: unrecoverable exception: <message>` and the stack trace to stderr, then calls `abort()`. The abort generates SIGABRT for core-dump or debugger inspection. An uncaught alarm is itself a fatal condition.
- If a fiber's body throws `UnrecoverableException`, the runtime logs it and calls `abort()` on the entire process. Per-fiber recovery from an unrecoverable is not safe, because the runtime invariant has been violated, and propagating it through `await` would let the corruption hide.
- If a fiber's body throws `RecoverableException`, the runtime stores the exception on the `Task<T>`'s exception slot and signals done. The awaiter re-raises into its own frame when it does `await task`.

The implementation works through the vtable. Every class vtable carries a `parent_vtable` pointer at a fixed offset, `CAJETA_VTABLE_PARENT_OFFSET`, which is NULL at the root. Codegen in `Compiler::emitUnrecoverableMarker` emits a module global constructor that calls the runtime setter `__cajeta_set_unrecoverable_vtable(UnrecoverableException's vtable)` once at startup, stashing it in the runtime's `g_unrecoverable_vtable` static. The runtime helper `__cajeta_is_unrecoverable(void* throwable)` reads the instance's vtable from slot 0 and walks the `parent_vtable` chain upward, matching against `g_unrecoverable_vtable`. It returns 1 for any descendant of `UnrecoverableException` and 0 otherwise. It also returns 0 defensively for legacy bare-integer throws, whose "pointer" falls below the zero-page boundary. The global-ctor plus plain-call scheme resolves identically across ELF/MachO/COFF and in both JIT and AOT.

User code can `catch (UnrecoverableException e)` if it really wants to, because the language does not forbid it. The convention is to leave it alone unless you have an extremely good reason. A top-level supervisor in a long-running daemon may legitimately want to log and keep going for some kinds of unrecoverable. The system catch is the safety net for everyone else.

## Stack traces

Stack-trace capture is a single compiler-wide flag, `--stack-trace-capture=on|off`, and its default is `on`. When enabled, every throw site walks the native call stack via `backtrace(3)` on glibc and macOS and records the frames on the thrown instance, whether it is recoverable or unrecoverable. There is no per-exception or per-construction opt-in or opt-out. The flag is read once and applies to all throws, and the runtime mirrors it through `__cajeta_set_stack_trace_capture` / `__cajeta_get_stack_trace_capture`.

```
cajeta --stack-trace-capture=off  program.cajeta   # disable capture (hot-loop throws)
```

Two practical caveats from the implementation:

- **Capture is skipped inside a fiber.** `backtrace(3)` walks the OS-thread stack. A parked or resumed fiber runs on a swapped `ucontext` stack where the native walk is not meaningful, so throws raised on a fiber do not carry a native trace today.
- The debug build presets turn capture off by default, since an attached debugger supplies the stack itself. Capture is `true` only in the default flag set.

If no trace was captured, because the flag was off or the throw happened on a fiber, the printed exception shows only the type, the message and the cause chain.

## Async / fiber integration

### Task<T> carries an exception slot

`CajetaTask`'s layout extends from `{ T value, i32 done }` to `{ T value, i32 done, Throwable* exception }`. The trampoline that runs a fiber wraps the inner call in a try:

- On normal completion, the trampoline stores the value and sets done.
- When the body throws a `RecoverableException`, the trampoline stores the exception and sets done. It does not propagate to the carrier OS thread.
- When the body throws an `UnrecoverableException`, the trampoline logs it and aborts the process.

`await` checks the exception slot on resume. If the slot is non-null, it re-raises into the awaiter's frame.

```cajeta
async int32 fetchScore(Url u) throws IOException {
    Response r = http.get(u);  // can throw IOException
    return r.statusCode;
}

public int32 caller() throws IOException {
    return await spawn fetchScore(u);  // IOException re-raised here at await
}
```

The `throws` clause on the async method documents what the awaiter might see, and the warning behavior at call sites still applies.

### scope and exception escalation (R5-D)

When a scope joins child tasks:

1. Walk every registered child and await each one through `__cajeta_scope_exit` / `__cajeta_scope_exit_to`.
2. The moment a child's exception slot is non-null, record it as the trigger.
3. Cancel every remaining still-running sibling. `__cajeta_fiber_cancel(fiber, trigger)` stores the trigger itself in each sibling's `cancel_with` marker. At the sibling's next `await` park-resume, `__cajeta_task_wait` re-raises that marker.
4. Keep waiting on the rest so the scope still joins everything before unwinding.
5. Re-raise the trigger's original exception to the scope's containing frame, unwrapped.

There is no `CancellationException` type. The fiber model removes the thread boundary that justified wrapping in older designs such as Java's `ExecutionException`. The trigger flows through await and scope the same way a thrown exception flows through any function call, and a cancelled sibling re-raises that same trigger Throwable. No synthetic cancellation marker is involved. Callers handle `catch (IOException e)` directly. If a sibling needs to know what triggered its cancel, the thrown trigger is that information, and the `cause` field on `Exception` is available for chaining if a handler rewraps.

**No `AggregateException`.** Several children failing at the same time is rare, and the caller usually wants one error to handle. If a use case ever demands collecting every child's outcome, a stdlib `scope.collectAll() -> List<Result<T>>` helper can be added without changing core semantics. It would build on top of the per-task exception slot the trampoline already populates.

**TLS hardening (shipped).** The setjmp/longjmp exception chain head, `__cajeta_exc_top`, and the drop-chain head were promoted off their single global slots. `cajeta_fiber` now carries per-fiber `exc_top` / `drop_top` fields, with `__thread`-qualified `__cajeta_main_exc_top` / `__cajeta_main_drop_top` for the main OS thread, selected through `__cajeta_exc_top_ptr()` / `__cajeta_drop_top_ptr()`. This is the same pattern `scope_top` uses. The change was necessary even under today's single-carrier cooperative model, because the carrier runs on a separate OS thread from `main`, so the two could race on a shared global. See `AsyncStatus.md`.

### Drop chain on the throw path

Owned locals drop on the throw path the same as on the return path. The existing drop-chain machinery with its watermark handles both, so no exception-specific bookkeeping is needed.

## Streams (forward-looking)

Cajeta does not yet have a stream API. This section pins the policy for when one lands, so that the design cannot drift toward a decorator model.

**Detailed spec.** See `StreamParallelism.ErrorHandling.md` for the parallel-stream walk-through from worker throw to scope join to trigger re-raise to drop chain, for the user-side tolerance patterns of lambda try/catch, a user-authored Outcome<T>, the mapOr* helpers and threshold abort, and for the explicit non-goals restated in a parallel context. The position below is the policy, and the companion doc is the specification.

The core position is that stream operations have no error-handling sub-DSL of their own. Lambdas inside `.map(...)`, `.filter(...)` and `.flatMap(...)` can throw like any other code. The exception propagates through the pipeline, and the caller wraps the terminal operation in `try/catch` if recovery is wanted.

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

Per-element recovery is a try/catch inside the lambda:

```cajeta
urls.stream()
    .map(u -> {
        try { return http.get(u).statusCode; }
        catch (IOException e) { return -1; }
    })
    .toList();
```

This is verbose for the per-element case, but it is explicit. A reader sees what is caught and where.

**Convenience helpers (stdlib only).** Where the verbose pattern is too common to write out, the stdlib can supply named operators that codify a specific recovery shape:

- `mapOrSkip(λ)` drops elements whose lambda threw `RecoverableException` and returns a shorter stream.
- `mapOrLog(λ, logger)` logs and drops.
- `mapOrFallback(λ, fallback)` replaces failed elements with a default.

These are opt-in by name. The pipeline's intent stays readable, because a reader does not have to scan a trailing `.onError(...)` decorator to discover that failures are being suppressed.

**Parallel streams.** They mirror scope's join semantics. The first throw wins, sibling workers get cancelled, and the trigger exception is re-raised at the terminal operation. There is no `AggregateException` at the stream layer either.

**Explicitly NOT in the design:**

- **Decorator chains** (RxJava-style `.onErrorReturn(...)` / `.onErrorResume(...)`). They live in the wrong place, separated from the producing operation that failed, and they flatten per-step context.
- **Stream-element `Try<T>` / `Result<T, E>` wrapping** (Vavr-style). It conflicts with our exception model, and mixing it into streams confuses users about which world they are in.
- **Stream-specific catch-by-step syntax** (`.map(λ).catch(IOException, λ).filter(λ)`). It treats the pipeline as a try/catch region, but a pipeline is a sequence of transformations applied to elements.

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

`AssertionError extends UnrecoverableException`. The caller does not catch it, and if it is hit the process aborts with a stack trace.

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

If any spawned `fetchScore` throws, the `await` re-raises into the surrounding frame. Control leaves the loop. `scope` cancels the remaining children, waits for their unwinds, then propagates the exception up to the caller of `aggregateScores`. `total` drops on the throw path the same as on a normal return.

### Example 5: suppressing the warning

```cajeta
@SuppressLint("uncaught-throws")
public void crashOnPurpose() {
    riskyOp();   // declared throws WeirdException; we want it to propagate
}
```

This is used when a method is intentionally a thin pass-through and the noise of a documenting throws clause is not worth it.

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

- **C++-style unchecked exceptions.** There is no signature documentation, the ABI contracts are silent, and teams either ban exceptions entirely or wrap everything. We keep the documentation value without the enforcement penalty.
- **Java's enforced checked exceptions.** The `throws` cascade is the well-documented failure mode. Refactors force noise into every caller, lambdas cannot carry checked exceptions cleanly, and teams wrap everything as `RuntimeException` to escape. We retain Java's documentation idea and drop the enforcement.
- **C#/Java unchecked-only.** There is no compile-time signal at all, so readers learn what a method throws by reading the source or testing it. We add the throws clause back as documentation.
- **Rust `Result<T, E>` + `?`.** Value-typed errors are clean, but they force a different control-flow paradigm. They also have real cost, since every call site has the `?` branch and every fallible function's result has the sum tag at the ABI level. For Cajeta, the syntactic familiarity of try/catch and the zero happy-path cost of the existing setjmp/longjmp infrastructure are stronger arguments. We may add a stdlib `Result<T, E>` later for hot paths where exception unwinding is too expensive.
- **Zig `E!T`.** The critique is the same as for Rust, plus the operator-order debate we already went through.
- **Swift `throws` (untyped) + `try`.** This is closer to what we want, but Swift's choice to type-erase the error sacrifices the documentation value. We keep the type list.
- **Effect systems.** They are academic and are not worth the cognitive cost in v1.

## Deferred / out of scope (v1)

- **`Result<T, E>` stdlib pattern.** This would give optional value-typed error returns for hot paths where exception unwind cost matters. It is a sealed sum plus helpers, with no compiler magic. It can be added when measurements demand it.
- **`@Throws(infer)`.** This would give compiler-inferred throws lists. It is a possible future ergonomic, and today the developer writes the list.
- **`when` guards in catch arms** (`catch (IOException e) when (e.code == EBUSY)`). They can be added if patterns demand it.
- **Type-scoped suppression.** This would suppress the `uncaught-throws` warning for only specific exception types instead of the whole method. Today `@SuppressLint("uncaught-throws")` is all-or-nothing for the annotated declaration.

## Known gaps (v1 spec → implementation)

This document is the spec. The v1 surface has shipped, and the status against the original punch list follows:

- [x] Stdlib: declare the four roots `Throwable` / `Exception` / `RecoverableException` / `UnrecoverableException` in `cajeta.error`. The built-in leaf subtypes (`AssertionError`, `OutOfMemoryError`, a unified `IOException`/`TimeoutException`/`CancellationException` set) are not yet declared. Domain exceptions live in their owning packages instead (`cajeta.io.file.IoException`, `cajeta.io.net.*`, …). A common prelude set remains open.
- [x] Parser: `throws` clause on method declarations, stored on `Method` as `throwsList`, advisory only.
- [x] Type-checker / lint: the `uncaught-throws` lint walks the called method's throws clause and an enclosing-try stack, suppressing the warning when a catch arm covers the type, with supertypes considered, or when it is declared or suppressed. Suppression is `@SuppressLint("id")`, generalized from `@SuppressUncaughtThrow`.
- [x] Runtime: exception path carries a `Throwable*` over a `void*` carrier, migrated off the bare `int64`.
- [x] Codegen: stack-trace capture via the global `--stack-trace-capture` flag, default on, for all throws. The per-kind opt-in and opt-out originally sketched here was dropped.
- [x] System default catch: wraps `main()` and each fiber trampoline. Recoverable and unrecoverable are distinguished by the `__cajeta_is_unrecoverable` vtable walk.
- [x] Async integration: `CajetaTask` carries an exception slot. The trampoline stores a recoverable throw in it and `await` re-raises into the awaiter. An unrecoverable aborts.
- [x] R5-C (cancellation): a per-fiber `cancel_with` marker holds the trigger Throwable and is checked on await resume. There is no `CancellationException` type, and the trigger itself is re-raised.
- [x] R5-D (scope exception escalation): `scope_exit` inspects each child's exception slot. The first thrower wins, siblings are cancelled with the trigger, and the trigger re-raises into the containing frame.
