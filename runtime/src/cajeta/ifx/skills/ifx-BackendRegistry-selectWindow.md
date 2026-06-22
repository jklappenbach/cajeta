---
id: ifx-BackendRegistry-selectWindow
applies-to: [cajeta/ifx/BackendRegistry.selectWindow]
title: BackendRegistry.selectWindow — bind the window backend (headless vs loud-fail policy)
description: Selects the window backend at launch; the headless flag decides silent null-floor bind vs loud IfxException, with CAJETA_IFX_WINDOW force-override.
---

# `BackendRegistry.selectWindow(boolean headless)`

```
public WindowBackend selectWindow(boolean headless)
```

Binds and returns the window backend for this launch. Pass `headless = true` when your
app opts into offscreen rendering and accepts a black-hole window; pass `false` when you
need a real interactive window. **That single flag is the whole protocol** — it decides
whether a missing real backend is tolerated silently or raised loudly.

## What it returns / decides

Selection runs in this order:

1. **`CAJETA_IFX_WINDOW=<name>` set** → force the registered backend whose `name()` equals
   `<name>`, bypassing `probe()`/`priority()` *and* the headless policy. An unknown name is
   a loud launch error (`IfxException`) **even when `headless == true`** — the operator
   named something that isn't there, so it never silently falls through.
2. **A real backend is viable** → bind the highest-`priority()` one whose `probe()` returns
   true. Ties resolve to the **first-registered** (the loop replaces the incumbent only on a
   *strictly greater* priority).
3. **Only the null floor (`name() == "null"`) is viable, or nothing is** →
   - `headless == true`  → return the floor silently (or `null` if no backend at all is registered).
   - `headless == false` → `throw heap IfxException(...)` rather than hand back a silent black screen.

Forcing `CAJETA_IFX_WINDOW=null` is therefore an explicit opt-in to the floor regardless of
`headless`.

## Ownership / lifecycle

The returned `WindowBackend` is a **borrowed** interface reference — it is owned by the
registry's internal `ArrayList<WindowBackend>` (the backend was handed over with `heap` at
`registerWindow`). Do **not** drop or free it; do not store it past the registry's lifetime.
There is no `#` transfer on this return. (Arrays returned by the backend's own methods —
`poll`/`pollLifecycle` — *are* `#`-transferred; that is the `WindowBackend` class contract,
not this method's.)

## Failure mode

`IfxException` (a `RecoverableException`, so catchable/testable) in exactly two cases:
unknown `CAJETA_IFX_WINDOW` name, or an interactive (`headless == false`) request when only
the null floor is viable. Leave it uncaught in a real app — it propagates to the top-level
handler and aborts loudly at launch, which is the intended behavior.

## What it does NOT do

- It does **not** create a window or surface — it only *binds* the backend. Call
  `createWindow(...)` on the returned backend afterward.
- It does **not** read the env override for input/audio — those are `selectInput()` /
  `selectAudio()`, which have **no loud-error case** (an empty/silent floor is a valid
  headless outcome, so they just return the best viable backend, never throw).
- Window/input/audio are selected **independently** from separate per-domain registries.

## Example

Idiomatic launch path (mirrors `IfxRegistryTests` 4d/4e and the singleton path):

```cajeta
import cajeta.ifx.BackendRegistry;
import cajeta.ifx.WindowBackend;
import cajeta.ifx.IfxException;
import cajeta.ifx.Window;

// instance() auto-registers the Null* floor on first touch, so this never comes up empty.
BackendRegistry registry = BackendRegistry.instance();

try {
    // headless = false: demand a real interactive window; only the null floor present → throws.
    WindowBackend window = registry.selectWindow(false);   // borrowed; owned by the registry
    Window w = window.createWindow("App", 1280u, 720u);
    // ... render loop ...
} catch (IfxException e) {
    // No cajeta-ifx-<os> linked, or a bad CAJETA_IFX_WINDOW name. Loud, logged, testable.
}
```

For a server/offscreen tool, pass `true` to accept the floor silently:

```cajeta
WindowBackend window = BackendRegistry.instance().selectWindow(true);  // floor OK, no throw
```

See the `WindowBackend` interface for what the bound value can do, and `IfxException` /
`NullWindowBackend` for the floor and error contract.
