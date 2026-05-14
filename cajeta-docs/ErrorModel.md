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

```
Throwable                           (abstract root)
├── UnrecoverableException          (the alarm — terminates the process)
│   ├── AssertionError
│   ├── OutOfMemoryError
│   ├── StackOverflowError
│   └── ...
└── RecoverableException            (caller may handle)
    ├── IOException
    │   ├── FileNotFoundException
    │   └── TimeoutException
    ├── ParseException
    └── ...
```

- `Throwable` is the common base — anything that can be thrown.
- `UnrecoverableException` is for conditions the program has no plan for: assertion failures, exhausted memory, contract violations, unreachable branches. Throwing one terminates the process (after the drop chain unwinds).
- `RecoverableException` is for failures the caller is expected to deal with: I/O errors, parse failures, timeouts, business-rule violations.
- User-defined exceptions extend one or the other. The choice is a design decision the author makes when defining the exception type.

## Signatures

A method that can throw a `RecoverableException` subtype declares it in its `throws` clause:

```cajeta
public int32 readPort(int32 port) throws IOException {
    if (port < 0) {
        throw new IOException("invalid port: " + port);
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

The warning is suppressible with an annotation:

```cajeta
@SuppressUncaughtThrow
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

No `finally` keyword — the drop chain already handles "run this cleanup on any exit path." If you need explicit cleanup, declare the resource as an owned local; its drop fires on every exit (return, throw, fall-through).

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
throw new IOException("disk full");
```

`throw` is a statement (like `return`). The exception value must be an instance of `Throwable` (or subtype). The drop chain unwinds owned locals on the way up.

## Unrecoverable exceptions and the system default catch

Unrecoverable exceptions are the alarm. The runtime wraps `main()` (and each spawned fiber's entry trampoline) in a default catch:

- If `main()` exits via an uncaught `RecoverableException`: the runtime logs the type + message + (if available) stack trace to stdout, then returns a nonzero exit code. The process exits cleanly.
- If `main()` exits via an uncaught `UnrecoverableException`: same logging, but the process aborts (nonzero exit). The "uncaught alarm" is itself a fatal condition.
- If a fiber's body throws `UnrecoverableException`: log + abort the entire process. Per-fiber recovery from unrecoverables isn't safe — the runtime invariant has been violated.
- If a fiber's body throws `RecoverableException`: store the exception on the `Task<T>`'s exception slot, signal done. The awaiter re-raises into its own frame when it does `await task`.

User code can absolutely `catch (UnrecoverableException e)` if it really wants to — the language doesn't forbid it. The convention is "don't, unless you have an extremely good reason" (a top-level supervisor in a long-running daemon may legitimately want to log-and-keep-going for some kinds of unrecoverable). The system catch is the safety net for everyone who doesn't.

## Stack traces

Stack-trace capture is **opt-out for `UnrecoverableException`** (default: captured at construction) and **opt-in for `RecoverableException`** (default: not captured).

Rationale: unrecoverables are rare and worth debugging — paying the stack-walk cost is fine. Recoverables can be thrown in hot loops (parser fast-fail, retry idioms) where the per-throw cost matters. Devs who want the trace on a specific recoverable can request it at construction:

```cajeta
throw new IOException("disk full", captureTrace: true);
```

If no trace was captured, the printed exception just shows type + message + cause chain.

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

1. Walk every registered child; await each.
2. If any child threw, collect the exception(s).
3. Cancel the remaining children (R5-C) and wait for them to unwind.
4. Re-raise the first exception into the scope's containing frame.

Cancellation surfaces as a `CancellationException extends RecoverableException` raised at the next `await` resume.

### Drop chain on the throw path

Owned locals drop on the throw path the same as on the return path. The existing drop-chain machinery (with the watermark) handles both; no exception-specific bookkeeping needed.

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
    // Suppress with @SuppressUncaughtThrow, or add it to the throws clause,
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
    throw new AssertionError("vtable hash not found — caller invoked a method not in this vtable");
}
```

`AssertionError extends UnrecoverableException`. The caller doesn't catch; if hit, the process aborts with a stack trace.

### Example 4: async + scope

```cajeta
public async int32 aggregateScores(List<Url> urls) throws IOException {
    int32 total = 0;
    scope {
        for (Url u in urls) {
            total = total + await spawn fetchScore(u);   // fetchScore throws IOException
        }
    }
    return total;
}
```

If any spawned `fetchScore` throws, the `await` re-raises into the surrounding frame. Control leaves the loop; `scope` cancels the remaining children, waits for their unwinds, then propagates the exception up to the caller of `aggregateScores`. `total` drops on the throw path the same as on a normal return.

### Example 5: suppressing the warning

```cajeta
@SuppressUncaughtThrow
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
- **Conditional `@SuppressUncaughtThrow(IOException, TimeoutException)`** — suppress only specific types. Today the annotation is all-or-nothing.

## Known gaps (v1 spec → implementation)

This document is the spec. Implementation lands incrementally:

- [ ] Stdlib: declare `Throwable`, `UnrecoverableException`, `RecoverableException` + a small set of built-in subtypes (`AssertionError`, `OutOfMemoryError`, `IOException`, `TimeoutException`, `CancellationException`).
- [ ] Parser: `throws` clause on method declarations (`methodDecl : ... THROWS qualifiedNameList`).
- [ ] Type-checker: parse and store the throws list on `Method`.
- [ ] Lint pass: for each call site, walk the called method's throws clause; emit warning if the type isn't caught by an enclosing try, declared on the enclosing method's throws clause, or suppressed via `@SuppressUncaughtThrow`.
- [ ] Runtime: extend the existing setjmp/longjmp infrastructure to carry a `Throwable*` rather than a bare `int64`. (Already mostly there.)
- [ ] Codegen: stack-trace capture path for `UnrecoverableException` (default) and opt-in for `RecoverableException`.
- [ ] System default catch: wrap `main()` and each fiber trampoline in a try that distinguishes recoverable from unrecoverable.
- [ ] Async integration: extend `CajetaTask` layout with an exception slot; trampoline stores recoverable in the slot, propagates unrecoverable; `await` re-raises.
- [ ] R5-C (cancellation): `CancellationException` subtype + per-task cancel flag; await checks on resume.
- [ ] R5-D (scope exception escalation): scope_exit on join inspects each child's exception slot; first thrower wins; cancel siblings; re-raise.
