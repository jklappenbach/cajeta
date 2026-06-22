---
id: ifx-IfxException
applies-to: [cajeta/ifx/IfxException]
title: IfxException — ifx's loud launch-failure signal
description: The RecoverableException BackendRegistry throws when an interactive window has no real backend, or a CAJETA_IFX_* override names an unknown backend.
---

# IfxException

A **support/exception type** in the `cajeta.ifx` package (library `cajeta.ifx`).
You do **not** construct it in app code — `BackendRegistry` throws it for you. As a
caller you either let it propagate (the intended launch-time abort) or `catch` it to
observe/assert the failure. It is a `RecoverableException` (catchable; it will not
abort on its own unless it reaches the top-level handler uncaught).

## When it fires — exactly two cases

`BackendRegistry` raises `IfxException` only as a *loud launch error*, and only in
these cases:

1. **Interactive window with no real backend.** `selectWindow(false)` (interactive
   intent) when the only viable window backend is the always-present null floor.
   Binding the floor would give a silent black screen, so the registry refuses.
   `selectWindow(true)` (opt-in headless) does **not** throw — it binds the floor.
2. **Unknown forced backend name.** A `CAJETA_IFX_WINDOW` / `CAJETA_IFX_INPUT` /
   `CAJETA_IFX_AUDIO` env value that names no registered backend in that domain.
   This throws even for a headless request (the unknown name is checked first).

It does **not** fire for input or audio domain selection: an empty device set or a
silent output is a valid headless outcome, so `selectInput()` / `selectAudio()` never
throw a no-backend error.

## What it is NOT used for

- **Permission denials are never thrown.** A denied OS capability is reported as
  `PermissionState.Denied` (see `cajeta/ifx/PermissionState`) — the app reads the
  state and degrades; there is no exception path for it.
- It carries **no error code or cause** — only a human-readable `message` (inherited
  from `Throwable` via `cajeta.error.Exception`); `cause` is set to `0` (unset).
- It is not `Unrecoverable` — it stays catchable for launch harnesses and tests.

## Construction & ownership

```cajeta
public IfxException(String message)   // message stored into the inherited `message` field
```

Only `BackendRegistry` calls this (`throw heap IfxException("...")`). The instance is
heap-allocated at the throw site and owned by the exception-propagation machinery; a
`catch` block borrows it for the handler's scope. Read `e.message` inside the handler;
copy it out if you need it past the handler.

## Idiomatic use — observe the loud failure

```cajeta
import cajeta.ifx.BackendRegistry;
import cajeta.ifx.WindowBackend;
import cajeta.ifx.NullWindowBackend;
import cajeta.ifx.IfxException;

BackendRegistry r = heap BackendRegistry();
r.registerWindow(heap NullWindowBackend());   // only the silent floor is present
try {
    WindowBackend b = r.selectWindow(false);  // interactive intent → loud failure
    // unreachable when no real backend is linked
} catch (IfxException e) {
    log(e.message);   // "no ifx.window backend viable ... link cajeta-ifx-<os>, or opt into headless"
}
```

A real app is expected **not** to catch this in normal flow: left uncaught it
propagates to the top-level handler and aborts loudly at launch — that is the intended
behavior (logged and testable, never a silent black screen). Catch it only in launch
harnesses and tests that assert the failure.

## Related

- `cajeta/ifx/BackendRegistry` — the only thrower; `selectWindow(headless)` and the
  `CAJETA_IFX_*` override logic decide when this is raised.
- `cajeta/ifx/PermissionState` — the report-not-throw path for permission denials.
- `cajeta.error.RecoverableException` — the catchable base type.
