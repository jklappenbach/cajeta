---
id: error-UnrecoverableException
applies-to: [cajeta/error/UnrecoverableException]
title: UnrecoverableException — the fatal, abort-bound alarm throwable
description: Throw for invariant violations / OOM / programmer error; it bypasses user catch and aborts the process.
---

# UnrecoverableException — the alarm

**Throw this when the program cannot sanely continue** — invariant violation,
contract/assertion failure, out-of-memory, an unreachable branch taken. It is a
**support/exception type** in `cajeta.error`, a sibling of
`cajeta/error/RecoverableException` under `cajeta/error/Exception` →
`cajeta/error/Throwable`. Do **not** reach for it for failures a caller can handle
and proceed past (failed parse, missing record, transient I/O) — that is
`RecoverableException`.

The decisive difference is at runtime, not in the type alone: codegen registers
this class's vtable once at startup via a global constructor
(`__cajeta_set_unrecoverable_vtable`). On every `throw`, the runtime walks the
thrown instance's `parent_vtable` chain; if it reaches the registered vtable, the
throw is treated as unrecoverable and propagates **straight to `abort()`**,
**bypassing user `catch` handlers** and the per-fiber recovery path. Any type that
`extends UnrecoverableException` inherits this behavior (the chain walk matches
descendants). See `docs/specification/error/ErrorModel.md`.

## Construct & throw (idiomatic, test-backed)

```cajeta
import cajeta.error.UnrecoverableException;

public static int32 run() {
    UnrecoverableException u = heap UnrecoverableException("contract failure");
    throw u;
}
```

Or inline at the violation site:

```cajeta
import cajeta.error.UnrecoverableException;

if (slot == 0) {
    throw heap UnrecoverableException("allocator returned null slot");
}
```

`UnrecoverableException(#String message)` — the `#` means the constructor **takes
ownership** of `message`; the heap `String` transfers into the exception. `cause`
(inherited from `Exception`) is left unset (`0`); the inherited `message` field is
public and readable on a caught instance.

## Lifecycle & behavior

- **Throwing it terminates the process.** The drop chain still unwinds owned locals
  on the way up (same machinery as a return); then the runtime prints
  `cajeta: unrecoverable exception: <message>` plus the captured stack trace to
  stderr and `abort()`s (SIGABRT, for core-dump / debugger). A throw inside a fiber
  aborts the whole process — per-fiber recovery is deliberately unavailable.
- **Stack trace** is captured at the throw site when `--stack-trace-capture` is on
  (default on); it is not a per-exception option.

## What it does NOT do (avoid the dead ends)

- It does **not** appear in `throws` clauses — any method may throw it; it is the
  alarm, not part of a method's documented contract. Don't add it to a signature.
- It is **not** meant to be caught. The language permits `catch
  (UnrecoverableException e)` (a top-level daemon supervisor might), but it does
  **not** suppress the `uncaught-throws` lint for a sibling `RecoverableException`,
  and the convention is "don't, unless you have an extremely good reason."
- There is **no** `super(...)` call and the constructor does not chain a cause; set
  fields directly if a subtype needs more. No `AssertionError` / `OutOfMemoryError`
  leaf types ship yet — those are illustrative; throw `UnrecoverableException`
  directly or define your own subtype.
