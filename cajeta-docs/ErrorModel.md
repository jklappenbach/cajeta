# Cajeta Error Model — Specification v1

## Goals

- **Errors are values, not out-of-band control flow.** No setjmp/longjmp, no DWARF unwind tables for recoverable errors. An `Err(e)` flows back through the call stack as a return value — type-checked, pattern-matchable, transformable like any other value.
- **The signature carries one bit, not a list.** A function either can fail or it cannot. The shape of the error (`E`) is also part of the signature, but the caller's contract is "this can fail" — there's no Java-style `throws X, Y, Z` cascade that ossifies every refactor.
- **Forced acknowledgment at every call site.** Calling a fallible function without a `try` is a compile error. No silent propagation; the reader can always see where errors can flow out of an expression.
- **Two tiers.** Recoverable errors are typed values (`T!E`). Programmer bugs use `panic` — unrecoverable, no `try`/`catch`, terminates the fiber or program.
- **Composes with what already exists.** Owned values drop on the `Err` path the same way they do on the `Ok` path (drop chain handles both). Async tasks carry their result — `Ok` or `Err` — across the wait queue. Structured concurrency joins propagate the first `Err` up to the scope.

## Non-goals (v1)

- Exception hierarchies / inheritance-based catch (`catch (IOException)`). We catch by pattern, not by type-tree.
- Implicit conversion between error types (`Into`/`From` traits). User writes the conversion explicitly.
- Resumable exceptions / effect handlers. Algebraic effects are interesting but academic for v1.
- Multiple error types in one signature (`int32 ! (IoError | ParseError)`). One error type per fn; users compose via sealed sums when they need a union.
- Carry stack traces in `Err` values automatically. Add later if there's demand; today the `E` carries whatever the producer puts in it.

## Operator: `!` for fallible types

A function's return type can be written `T!E`, meaning "produces a `T` on success or an `E` on failure":

```cajeta
int32 ! IoError readPort(int32 port) {
    if (port < 0) {
        return Err(IoError.invalidPort(port));
    }
    return 42;  // implicit Ok wrap
}
```

- `T!E` is a sum type internally — conceptually `enum { Ok(T), Err(E) }` — but its surface ergonomics are tighter than writing the sum out by hand.
- A bare `return value` in a `T!E` function implicitly wraps the value as `Ok`.
- An explicit `Err(e)` constructs the failing variant.
- Functions that cannot fail are written as plain `T` (no `!`).

## `try` at call sites

Every call to a fallible function MUST be preceded by `try`:

```cajeta
int32 ! IoError useReadPort() {
    int32 v = try readPort(8080);  // propagates Err to caller if readPort fails
    return v + 1;
}
```

Without the `try`, the call is a compile error: `readPort` returns `int32!IoError`, and assigning that to `int32` without acknowledging the failure path won't type-check.

`try expr` has three forms:

1. **Bare `try expr`** — propagate. The current function must itself return a compatible `T!E`. If `expr` evaluates to `Err(e)`, the current function returns `Err(e)` immediately; otherwise the expression's value is the unwrapped `T`.
2. **`try expr catch pattern -> handler`** — single-arm catch. If `expr` is `Err(e)` and the pattern matches, the handler's value is used; otherwise the `Err` propagates (same rule as bare `try`).
3. **`try expr catch { pat1 -> ...; pat2 -> ...; }`** — multi-arm catch. Pattern-match on the error value. A `default ->` arm catches anything; without it, unmatched errors propagate.

The catch arms are ordinary Cajeta patterns — destructuring, variable binding, guards. No type-hierarchy dispatch.

## `panic` for unrecoverable bugs

`panic(message)` aborts the current fiber (or the whole program if called from the main thread before the executor takes over) with a diagnostic message. There is no `try`-style mechanism to catch a panic — by design.

```cajeta
int32 div(int32 a, int32 b) {
    if (b == 0) {
        panic("div() called with b == 0; caller failed precondition");
    }
    return a / b;
}
```

Use panic for:
- Assertion-style invariant violations.
- Unreachable branches (an impossible enum tag, an interpreter dispatching on a known-good opcode that wasn't one).
- Resource exhaustion the program has no plan for (failed mmap of the executor's task arena).

Do NOT use panic for:
- Bad user input → return `Err(...)`.
- I/O failure → return `Err(...)`.
- Anything the caller could reasonably want to handle differently.

The runtime distinguishes panics from normal returns via a process-wide signal (currently `abort()` after writing the diagnostic). When the fiber executor is involved, a panic terminates the current fiber cleanly — the fiber's drop chain still runs so owned resources release — but the panic propagates up the scope as a fatal condition (the surrounding `scope { }` terminates with its own panic rather than re-throwing as a recoverable error).

## Catch patterns

Multi-arm catch:

```cajeta
int32 robustRead() {
    int32 v = try readPort(8080) catch {
        IoError.Timeout(d) -> 0;          // bind the timeout duration, ignore it
        IoError.Closed -> -1;
        IoError.InvalidPort(p) -> {       // multi-statement arm
            log.warn("invalid port: " + p);
            -2
        };
        default -> {                       // catch-all; bind the error
            log.error("unexpected: " + it);
            -99
        };
    };
    return v;
}
```

The `default ->` arm matches any error. Without it, the unmatched error propagates (same rule as bare `try`).

`it` inside an arm is the bound error value; named bindings work too (`default e ->`).

## Interaction with async / scope / drop chain

### Async

An `async` function can be fallible:

```cajeta
async int32 ! HttpError fetchScore(Url u) {
    Response r = try await http.get(u);
    return r.statusCode;
}
```

`await` on a `Task<T!E>` produces a `T!E` value at the suspension point — same semantics as any other fallible return. `try await spawn fetchScore(u)` is the canonical pattern: `await` unwraps the Task, `try` unwraps the `!E` (propagating if `Err`).

### Scope

When the scope joins child tasks, each task's result is inspected. If any child completed with `Err`, the scope:

1. Cancels every other child still running (R5-C territory).
2. Waits for those cancellations to unwind.
3. Returns the first `Err` encountered up to the scope's own caller.

The user-visible shape: a `scope { ... }` block inside a `T!E` function can itself produce an `Err`; the surrounding function uses `try` (or doesn't, if it pre-catches inside the scope).

### Drop chain

`Err` doesn't change anything for drops. An owned local that exists at the point of an `Err` return drops the same way it would on an `Ok` return. The drop chain is path-agnostic — it just tracks owned bindings and fires them in LIFO order on scope exit, whether the exit carries `Ok`, `Err`, or `panic`.

## Examples

### Example 1: bare propagation

```cajeta
int32 ! ParseError parseTwoNumbers(String s) {
    int32 first = try parseInt(s.before(','));
    int32 second = try parseInt(s.after(','));
    return first + second;
}
```

If either `parseInt` returns `Err`, the function returns the same `Err` and the caller sees it.

### Example 2: single-arm catch with default value

```cajeta
int32 readWithDefault(int32 port) {
    return try readPort(port) catch _ -> 0;
}
```

`_` is the wildcard pattern. Any `Err` becomes `0`. The function's return type is `int32` (not fallible) because the catch handles every case.

### Example 3: pattern-matched recovery

```cajeta
int32 ! NetworkError robustFetch(Url u) {
    return try fetchScore(u) catch {
        HttpError.Timeout(_) -> try fetchScore(u);   // retry once
        HttpError.NotFound -> 404;                    // sentinel value
        HttpError.Unauthorized -> {
            try refreshAuth();
            try fetchScore(u)
        };
        default e -> return Err(NetworkError.upstream(e));   // re-wrap and propagate
    };
}
```

Multi-arm catch dispatches on the error's structure; the function is itself fallible (returns `int32 ! NetworkError`) so re-wrap-and-propagate via explicit `return Err(...)` is fine. The catch arms can themselves contain `try`s (nested fallibility composes).

### Example 4: async with structured concurrency

```cajeta
async int32 ! NetworkError aggregateScores(List<Url> urls) {
    int32 total = 0;
    scope {
        for (Url u in urls) {
            int32 partial = try await spawn fetchScore(u);   // any spawn's Err aborts scope
            total = total + partial;
        }
    }
    return total;
}
```

If any spawned `fetchScore` produces `Err`, the `try` propagates: control leaves the loop, the `scope` cancels the remaining children, waits for their unwinds, then returns the `Err` to the caller. The `total` local's drop fires in the unwind — same as on a normal `Err` return.

### Example 5: panic — bug, not error

```cajeta
int32 vtableLookup(VTable* v, int64 hash) {
    int32 lo = 0;
    int32 hi = v.entries.size() - 1;
    while (lo <= hi) {
        int32 mid = (lo + hi) / 2;
        int64 mhash = v.entries[mid].hash;
        if (mhash == hash) return mid;
        if (mhash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    panic("vtable hash not found — caller invoked a method not in this vtable");
}
```

The hash should always be present (vtable construction guarantees it); seeing a miss means a contract violation. Returning `Err` would force every call site to handle a case that should never happen.

### Example 6: try in async + lock

```cajeta
async int32 ! StorageError snapshot(Mutex<Db> db) {
    MutexGuard<Db> g = await db.lock();   // not fallible — lock always succeeds
    return try g.value.snapshotTo(buffer);  // snapshotTo can fail
}
```

`g`'s drop releases the lock on either path (`Ok` or `Err` from `snapshotTo`). The `try` propagates the storage error to the caller without any explicit cleanup code.

## Rationale (why not the alternatives)

- **C++-style unchecked exceptions** — silent ABI risk, the worst sin in a language that prides itself on visible control flow. Cajeta already has drop chains and a fiber model; grafting EH unwind tables on top would duplicate cleanup mechanisms for no benefit.
- **Java-style checked exceptions** — the `throws X, Y, Z` cascade is a pre-fact-checked-by-the-community failure. Lambdas become awkward, refactors cascade noise into every signature in the call tree, teams give up and wrap as `RuntimeException`. The dichotomy "type-system says fallible" → checked, "type-system silent" → unchecked is wrong: it should always be in the type system, but as a single bit, not a list.
- **Java-style unchecked / C#** — nicer ergonomics than checked, but the caller has to read documentation to know what to handle. Reliability suffers in libraries you don't own.
- **Go-style `if err != nil`** — errors are values (right!), but the boilerplate is brutal and there's no compiler enforcement that you check the error. We can do better with a `try` operator.
- **Rust `Result<T, E>` + `?`** — closest to what we're proposing, and has shaken out a lot of the ergonomics issues. We borrow the structure but tighten the surface syntax (the `T!E` form is more compact than `Result<T, E>`; `try` is more readable than the postfix `?` for users coming from other languages).
- **Swift `throws` / `try`** — also close. Swift's choice to type-erase the error (you don't say *what* you throw, just *whether* you throw) saves signature ink at the cost of giving up pattern-matched catches. We keep the error type explicit so multi-arm catches work without runtime introspection.
- **Algebraic effects** — academically appealing, but the ergonomics in production languages (Koka, Eff) aren't proven. Reserve for v2+ if `T!E` proves limiting.

## Deferred / out of scope (v1)

- **Error-type conversion sugar.** Today re-wrapping an inner error type into an outer one is explicit (`catch default e -> return Err(Outer.from(e))`). A `from` trait or a `try? as Outer` operator could shorten this; deferred until we see how painful the explicit form is in practice.
- **Stack traces in `Err` values.** Requires runtime cooperation (frame walk on Err construction). Pay the cost only when measurements demand it.
- **`finally` block.** The drop chain already handles "run this cleanup on any exit path"; an explicit `finally` would be redundant.
- **Catching panics.** Deliberate non-feature. If a panic isn't fatal, it should be an `Err`.

## Known gaps (v1 spec → implementation)

This document is the spec. Implementation lands incrementally:

- [ ] Parser: `T ! E` type form, `Err(...)` constructor, `try`/`catch` syntax, `panic` keyword.
- [ ] AST: `FallibleType`, `TryExpression`, `CatchClause`, `PanicStatement`, `ErrExpression`.
- [ ] Codegen: lower `T!E` to a sum-tagged struct; lower `try` to a check + propagate; lower `catch` to pattern-match dispatch.
- [ ] Runtime: `__cajeta_panic(msg)` — abort with diagnostic. Integrates with the fiber executor so a panic from inside a fiber terminates the fiber cleanly.
- [ ] Async integration: `Task<T!E>` carries the `Err` through the wait queue. await unwraps the `T!E` after the task completes.
- [ ] Scope integration (R5-D): joining children inspects each task's `Err` slot and re-raises the first one up the scope.
