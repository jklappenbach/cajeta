---
id: language-errors
applies-to: [cajeta/language/errors, cajeta/language/exceptions]
title: Error handling — advisory throws, the Throwable hierarchy, no try-with-resources
description: Java-shaped try/catch/finally where throws clauses warn instead of erroring; recoverable vs unrecoverable at the top; drop-on-scope is the resource pattern.
---

# Error handling

Java-style `try` / `catch` / `throw` with one deliberate change: **`throws`
clauses document, they do not enforce.**

## The hierarchy (`cajeta.error`)

`Throwable` (root; `message` + the diagnostics surface) → `Exception` (adds
`cause`) → `RecoverableException` (the caller may handle) and
`UnrecoverableException` (the alarm; the program shouldn't continue). Domain
exceptions extend one of these from their owning package
(`cajeta.io.file.IoException`, …), and so should yours.

**Mechanical note:** `super(...)` is not supported yet — a subclass
constructor assigns the inherited fields directly (`this.message = message;
this.cause = null;`).

## Checked in the advisory sense

A `throws` clause lists the `RecoverableException` subtypes that flow out.
A call site that neither catches nor declares them gets a compile **warning**
(`uncaught-throws`), never an error — so there is no Java-style refactor
cascade. Accept propagation deliberately with
`@SuppressLint("uncaught-throws")`. `UnrecoverableException` subtypes are
fully unchecked and never appear in `throws`.

## At the top, and on fibers

The runtime wraps `main()` and every fiber in a default catch: an uncaught
`RecoverableException` logs and exits 1; an uncaught `UnrecoverableException`
logs and `abort()`s (SIGABRT, core dump) — you *can* catch it, the convention
is don't. On fibers a recoverable throw is stored on the `Task` and re-raised
at `await`; a `scope` cancels siblings with the first failure and re-raises it
(`cajeta/language/concurrency`).

## No try-with-resources — and why you don't need it

There is no `try (R r = ...)` form; it was in the grammar and removed as
redundant. **Owned locals drop at the closing `}` in LIFO order, on return
*and* on unwind** — declare the resource as an owned local and cleanup is
guaranteed (`cajeta/language/ownership`). Reach for `finally` only when the
cleanup isn't tied to one owned value. (`@Cleanup` for close-before-drop is
planned, not implemented.)

## Diagnostics surface

Every `Throwable` carries `getMessage()`, `getCause()` (an `Optional`),
`getStackTrace()` (`StackFrame[]`, throw-site first, resolved from the
runtime's line-info shadow stack — no debug info needed),
`printStackTrace()`, and `toJson()` (one NDJSON object: severity, code,
message, category predicates, remediation, cause chain, frames). Pin a
refactor-proof identifier with `@DiagnosticCode("APP_ERR_…")`; the default
`code()` is the canonical type name. `--diag-format=json` puts the whole
toolchain on that wire format.

## Worked example (verified: returns 42, warning fires as documented)

```cajeta
package dev.cajeta.skills;

import cajeta.error.DiagnosticCode;
import cajeta.error.RecoverableException;
import cajeta.lang.System;

@DiagnosticCode("APP_ERR_BAD_PORT")
public class BadPortException extends RecoverableException {
    public BadPortException(#String message) {
        this.message = message;      // no super(...) yet — set inherited fields
        this.cause = null;
    }
}

public class Loader {
    public int32 load(int32 port) throws BadPortException {
        if (port < 0) {
            throw heap BadPortException("invalid port");
        }
        return port;
    }
}

public class ErrorsDemo {
    public static int32 run() {
        Loader l = stack Loader();
        int32 recovered = 0;
        try {
            recovered = l.load(-1);
        } catch (BadPortException e) {
            System.stdout.println("recovered: " + e.getMessage());
            recovered = 40;
        } finally {
            System.stdout.println("attempt finished");   // every exit edge
        }
        int32 ok = l.load(2);        // advisory: warns, still compiles
        return recovered + ok;       // 42
    }
}
```

Catch arms dispatch in source order, most-specific first. Stdlib error
hierarchy detail: `error-overview` and `error-exception-hierarchy`.
