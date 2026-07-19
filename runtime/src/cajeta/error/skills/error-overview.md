---
id: error-overview
applies-to: [cajeta.error]
title: cajeta.error — exception hierarchy orientation & routing
description: Pick Recoverable vs Unrecoverable vs ClassCastException, transfer the #String message on throw, chain causes, and how vtable-walk decides what aborts.
---

# cajeta.error — orientation & routing

`cajeta.error` is the throwable hierarchy: `Throwable` → `Exception` →
`{RecoverableException, UnrecoverableException}`. Every catchable type derives
from it, and the runtime uses it to decide, on each `throw`, whether the error is
*recoverable* (catchable, propagates normally) or *unrecoverable* (the alarm —
bypasses user `catch` and aborts the process). Reach here whenever you need to
throw, define a domain exception, or decide which root to extend.

## Task → entry point

| Want to… | Use |
| --- | --- |
| Throw a failure a caller can handle and proceed past | `throw heap RecoverableException("...")` (or a subclass) |
| Signal a fatal invariant violation / programmer error / OOM | `throw heap UnrecoverableException("...")` |
| Define a domain exception | `class FooException extends RecoverableException` (or `UnrecoverableException`) |
| Catch everything throwable | `catch (Throwable e)` — the catch-all root |
| Read the failure message | `e.message` (a `String`) |
| Walk the chain of underlying causes | follow `e.cause` until it is `0` (see below) |
| Signal a failed runtime capture cast `(Foo<int32>) w` | `ClassCastException` — thrown *for you* by the runtime |
| **NOT** here: a `throws` clause that the compiler *enforces* | it doesn't — `throws` is documentation; an uncaught declared type is a **warning** (`uncaught-throws`), never an error |
| **NOT** here: `Result<T,E>` / `?` propagation / `T!E` value-typed errors | not in v1; use `try`/`catch` |
| **NOT** here: a constructor that takes a `cause` | none exists — set `e.cause` by field assignment after construction |
| **NOT** here: leaf types like `IOException`, `ParseException`, `OutOfMemoryError` | illustrative only, not in the prelude; domain leaves live in their owning package (`cajeta.io.file.IoException`, `cajeta.io.net.*`, …) |

## Cross-cutting invariants (whole library)

- **Message ownership transfers in.** Every root constructor takes
  `#String message` — ownership moves into the exception on construction
  (`ClassCastException` is the lone exception: it takes a borrowed `String`).
  You do not free the message afterward; the exception owns it.
- **Throw form.** `throw` is a statement and takes a heap instance:
  `throw heap RecoverableException("...")` (`new #` and `heap` forms both appear
  in the tree; prefer `heap`). The value must be a `Throwable` or subtype.
- **`cause` is null-as-`0`.** `Exception` adds `public Throwable cause`, default
  `0` (none). There is **no cause constructor** — chain by assigning after you
  build: `wrapper.cause = lower;`. Walk until `cause == 0`.
- **No `super(...)` calls.** Constructors write inherited fields directly
  (`this.message = message; this.cause = 0;`) because `super` is still
  unsupported — relevant only when you subclass and write your own constructor.
- **Drop chain unwinds on throw.** Owned locals drop on the throw path exactly
  as on the return path; prefer owned locals over `finally` for cleanup.
- **Default catch at every entry point.** `main()` and each fiber are wrapped:
  an uncaught `RecoverableException` logs + exits 1 (clean); an uncaught
  `UnrecoverableException` logs + `abort()`s (SIGABRT). Programs never crash
  silently.

## Recoverable vs Unrecoverable — the detection model

The split is *not* a flag on the instance — it is **vtable identity**. Codegen
emits a global constructor (`__cajeta_set_unrecoverable_vtable`) that registers
`UnrecoverableException`'s vtable once at startup. On a throw the runtime
(`__cajeta_is_unrecoverable`) reads the instance's vtable and walks the
`parent_vtable` chain upward: if it reaches the registered address, the throw is
unrecoverable and bypasses user `catch` to propagate to abort; otherwise it is an
ordinary catchable throw. Consequence: **any** subclass of `UnrecoverableException`
is unrecoverable automatically — choose the root you extend deliberately, because
that choice *is* the recoverability. (User code *can* `catch (UnrecoverableException
e)`, e.g. a daemon supervisor, but the convention is don't.)

## Disambiguation

- **`RecoverableException`** — failures a caller has a sensible fallback for: bad
  parse, missing record, transient I/O, business-rule violation. The default
  choice for a domain exception.
- **`UnrecoverableException`** — conditions with no recovery plan: invariant
  violation, contract breach, unreachable branch, out-of-memory. Throwing one
  ends the process after the drop chain unwinds.
- **`ClassCastException`** (extends `RecoverableException`) — thrown *by the
  runtime* from an unguarded capture cast `(Foo<int32>) w` when the reified
  instantiation doesn't match. You normally don't throw it; to avoid it entirely
  use the guarded form `if (w instanceof Foo<int32> f) { ... }`.

## Canonical example

```cajeta
import cajeta.error.RecoverableException;
import cajeta.error.Exception;

public class Records {
    // `throws` documents; it is not enforced (uncaught => warning only).
    public Record parse(String line) throws RecoverableException {
        if (line.length() == 0) {
            throw heap RecoverableException("empty record line");
        }
        return decode(line);
    }

    public Record parseOrDefault(String line) {
        try {
            return parse(line);
        } catch (RecoverableException e) {
            log(e.message);          // message is owned by e
            return Record.empty();
        }
    }

    // Catch-and-rewrap: chain the underlying failure via the cause field.
    public Config load(String path) throws RecoverableException {
        try {
            return readConfig(path);
        } catch (RecoverableException low) {
            RecoverableException wrap = heap RecoverableException("config load failed: " + path);
            wrap.cause = low;        // no cause ctor — assign the field
            throw wrap;
        }
    }
}
```

## Setup

The four roots live in `package cajeta.error;` and are part of the
implicitly-loaded prelude. `import cajeta.error.<Type>;` to name a specific one.
Stack-trace capture is a compiler-wide flag (`--stack-trace-capture=on|off`,
default on); it is skipped for throws raised on a fiber.

## Downward pointers

These are simple value/identity types — see the source headers in
`runtime/src/cajeta/error/` (`Throwable`, `Exception`, `RecoverableException`,
`UnrecoverableException`, `ClassCastException`) for the exact constructor
signatures. The full model is `docs/specification/error/ErrorModel.md`.
