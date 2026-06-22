---
id: error-exception-hierarchy
applies-to: [cajeta/error/Throwable, cajeta/error/Exception, cajeta/error/RecoverableException, cajeta/error/UnrecoverableException]
title: Base exception hierarchy (Throwable / Exception / Recoverable / Unrecoverable)
description: How the four cajeta.error roots cooperate — message root, cause chain, the recoverable/unrecoverable split the runtime resolves by vtable chain-walk, and the no-super constructor pattern subclasses must copy.
---

# The base exception hierarchy

Four cooperating classes in `cajeta.error`, all auto-loaded by the stdlib prelude (referenceable by simple name, no import needed):

```
Throwable                       root identity — carries `message`
└── Exception                   adds `cause` link (0 = none)
    ├── RecoverableException    caller may handle and continue
    └── UnrecoverableException  the alarm — propagates past catch to abort
```

**Routing — pick the type you throw:**
- A failure the caller can sensibly handle (parse error, missing record, transient I/O) → throw `RecoverableException` or a domain subclass of it.
- An invariant violation / programmer error / OOM the program can't continue past → throw `UnrecoverableException`.
- Catch-all type in a `catch` arm → `Throwable` (or `Exception`). `Throwable` is the root identity for "anything thrown"; `Exception` is the lowest type that has the `cause` field.
- Bare `Throwable` / bare `Exception` are rarely *thrown* directly — in practice every throw is a Recoverable or Unrecoverable. Use them as catch-target supertypes, not as the thrown value.

## Members and roles

- **`Throwable`** — root. One field `public String message`. Constructor `Throwable(#String message)`.
- **`Exception extends Throwable`** — adds `public Throwable cause` (the wrapped lower-level failure, `0` when none). Constructor `Exception(#String message)` sets `this.cause = 0`.
- **`RecoverableException extends Exception`** — no new fields; same `(#String message)` constructor. The catchable tier.
- **`UnrecoverableException extends Exception`** — no new fields; same `(#String message)` constructor. The fatal tier; its vtable is what the runtime matches against.

## Collaboration / object graph

Two cooperating mechanisms, both keyed off the vtable chain every instance carries:

1. **The `cause` chain (data).** `Exception.cause` is a `Throwable` pointer forming a "X caused by Y caused by Z" linked list. It is `0`-terminated, not null-via-Optional — check `e.cause != 0` before walking. A bare throw leaves `cause = 0`; a rewrap site assigns `this.cause = caught` to chain.
2. **The recoverable/unrecoverable split (control).** The split is *not* a flag on the instance — the runtime resolves it structurally. At startup, codegen emits a global constructor (`__cajeta_set_unrecoverable_vtable`) that registers `UnrecoverableException`'s vtable. On a throw, the runtime helper `__cajeta_is_unrecoverable` reads the thrown instance's vtable (slot 0) and walks the `parent_vtable` chain upward, matching that address. Any descendant of `UnrecoverableException` matches and bypasses user `catch` arms straight to `abort()`; everything else (including `RecoverableException` and its subclasses) stays catchable. **Consequence:** you make a domain error fatal-vs-catchable purely by *which root you extend* — there is no field to set, and subclasses inherit tier membership automatically through the vtable parent chain.

## The no-super constructor pattern (the sharp edge)

`super(...)` is unsupported (`UnsupportedExpression`), so constructors **do not** chain. Each subclass assigns the inherited fields directly and must repeat them. A correct subclass constructor sets **both** `this.message` and `this.cause`:

```cajeta
package myapp.io;

public class IOException extends RecoverableException {
    public IOException(#String message) {
        this.message = message;   // inherited from Throwable
        this.cause = 0;           // inherited from Exception — MUST set; no super runs
    }
}
```

Forgetting `this.cause = 0` leaves it uninitialized — there is no base constructor to default it. To chain a cause, add an overload that assigns `this.cause = cause` instead of `0`.

## Ownership across the boundary

- The `#String message` parameter is an **ownership transfer** (`#`): the string moves into the exception and the exception now owns it. Pass a freshly built/owned string; don't reuse it after the throw.
- Exceptions are thrown as heap values: `throw heap RecoverableException("...")`. On unwind the drop chain frees the instance (and its owned `message`) along the throw path, the same way it unwinds owned locals on a normal return.
- `cause` is a borrowed-then-owned link: when you assign a caught exception into `this.cause` you are extending its lifetime into the wrapper's graph — don't also let it drop separately.

## Worked example — throw, chain-walk dispatch, catch

```cajeta
package myapp;

// Throwable/Exception/RecoverableException/UnrecoverableException need no import
// (stdlib prelude). Domain subclasses you define are referenced normally.

public int32 loadConfig(#String path) {
    try {
        return parseConfig(path);            // may throw RecoverableException
    } catch (RecoverableException e) {
        log(e.message);                      // recover: caught, program continues
        if (e.cause != 0) {                  // 0-terminated cause chain, not Optional
            log(e.cause.message);
        }
        return -1;
    }
    // An UnrecoverableException thrown below `parseConfig` would NOT be caught
    // here even by `catch (Throwable)` in practice — the runtime's vtable
    // chain-walk routes it past all user catch arms to abort.
}

public void assertSlot(int32 slot) {
    if (slot == 0) {
        throw heap UnrecoverableException("allocator returned null slot");
    }
}
```

## When to use which / what this does NOT do

- Extend `RecoverableException` for anything a caller is expected to handle; extend `UnrecoverableException` for the alarm. The choice is made once, at type-definition time, by the `extends` target.
- This hierarchy does **not** enforce `throws` clauses — they document and warn, never block compilation (no Java-style checked-exception cascade).
- There is **no** `CancellationException` and **no** `AggregateException` — fiber/scope cancellation re-raises the original trigger `Throwable` through `await`, not a synthetic wrapper.
- These four are the only types that ship in `cajeta.error`. Leaf types like `IOException`/`ParseException` are not in the prelude — they live in their owning packages, each extending one of these roots. For the `catch`/`throw`/`finally`/`throws` statement semantics and async `await` re-raise, see `docs/specification/error/ErrorModel.md`.
