---
id: ifx-BackendRegistry
applies-to: [cajeta/ifx/BackendRegistry]
title: BackendRegistry — process-wide ifx backend registry, probe, and dispatcher
description: How to register, select, and feature-detect ifx window/input/audio backends through the shared registry.
---

# BackendRegistry

The single access point for ifx backend selection. It is a registry + probe + dispatcher:
backends register themselves per domain, and at launch the registry binds, **per domain
independently**, the highest-`priority()` backend whose `probe()` reports it viable. ifx
depends on no backend (dependency inversion) — this class is how the app reaches whatever
is linked. This is a "start here" entry point, not a value type.

For most code the entry is `BackendRegistry.instance()`; you rarely construct one.

## Getting an instance

- `BackendRegistry.instance()` — the **lazily-built, process-wide** registry. On first
  touch it auto-registers the always-present `NullWindowBackend` / `NullInputBackend` /
  `NullAudioBackend` floor, so `import` + a headless program just works and selection never
  comes up empty. Use this everywhere.
- `heap BackendRegistry()` — a fresh, **empty** registry (no floor). Only for tests that
  want a controlled set. You must register a floor yourself or `selectInput()`/
  `selectAudio()` can return null and `selectWindow()` may throw.

No `close()`/`dispose()`; the shared instance lives for the process. There is no
de-registration and no reset.

## Routing — pick the method for your task

| Task | Call |
| --- | --- |
| Bind the window for this launch | `selectWindow(boolean headless)` |
| Bind input devices | `selectInput()` |
| Bind audio output | `selectAudio()` |
| A backend registers itself at load | `registerWindow/registerInput/registerAudio(backend)` |
| Feature-detect against the *bound* backend | `supportsWindow/supportsInput/supportsAudio(Feature)` |

There is **no** generic `select(domain)` or `register(domain, backend)` — the three
domains have separate, typed methods on purpose (a server can bind real audio with only the
null window floor). There is no remove/unregister and no list-all accessor.

## Selection contract

- Highest `priority()` among `probe()`-viable backends wins; the null floor is lowest.
- Priority ties resolve to the **first-registered** backend (registration order is
  load-bearing — the scan replaces the incumbent only on *strictly greater* priority).
- `probe() == false` is never bound, even at higher priority.
- `selectWindow` enforces a three-cases policy: a real backend binds; only the null floor +
  `headless == true` binds the floor silently; only the null floor + `headless == false`
  (interactive) **throws `IfxException`** rather than hand back a silent black screen.
- `selectInput`/`selectAudio` have **no loud-error case** — an empty device set / silent
  output is a valid headless outcome, so they just return the best viable backend (the floor
  at worst).
- Env override: each `select*` first consults `CAJETA_IFX_WINDOW` / `CAJETA_IFX_INPUT` /
  `CAJETA_IFX_AUDIO` (matched by `name()`); a value naming no registered backend throws
  `IfxException`. (The class doc notes this env path is still maturing — it depends on the
  pure-Cajeta env read; do not rely on it as the primary mechanism.)

## Ownership / lifecycle

- `register*(backend)` declares a **plain** interface formal (`WindowBackend backend`) and
  forwards it to a plain `ArrayList.add`, so the registry **holds a borrow**, not a title.
  **Do not write `#` at the call site.** A title handed to a plain formal is dropped when
  *that* frame returns rather than travelling on to the list, so `r.registerWindow(#backend)`
  frees the backend as `registerWindow` returns and leaves the registry holding a freed
  pointer (measured for exactly this shape — a plain formal forwarded plainly into
  `ArrayList.add`: the argument's destructor runs at the callee's return and the stored
  element reads back as garbage).
- **The backend must be owned by something that outlives every `select*` call.** For the
  same reason, a construction passed straight into the call —
  `r.registerWindow(heap Win32WindowBackend())` — is not self-sustaining either: the fresh
  temporary's title lands in `registerWindow`'s formal and drops at its return. Keep the
  instance in a binding that spans the process (a `main`-level local, a static) and lend
  that binding to `register*`. Never register a backend whose storage can go away — the
  registry keeps using it and nothing diagnoses a dangling entry.
- `select*()` and `supports*()` return a **borrowed** reference into the registry — owned by
  the registry, valid for the process. Do **not** free it.

## Example (idiomatic; the registry surface exercised by test/ifx/IfxRegistryTests.cpp)

```cajeta
import cajeta.ifx.BackendRegistry;
import cajeta.ifx.WindowBackend;
import cajeta.ifx.Feature;
import cajeta.ifx.IfxException;

// An OS backend registers itself at load (the null floor is already there via instance()).
// Hold the instance in a binding that outlives every select*/supports* call, and LEND it —
// no `#`, and not a temporary built inside the call.
Win32WindowBackend win32 = heap Win32WindowBackend();
BackendRegistry.instance().registerWindow(win32);

// The app binds for this launch. Interactive request: throws IfxException if only the
// silent floor is viable (no real cajeta-ifx-<os> linked).
WindowBackend window = BackendRegistry.instance().selectWindow(false);

// Feature-detect against the *bound* backend before using an optional capability:
if (BackendRegistry.instance().supportsWindow(Feature.MultiWindow)) {
    // ... use multi-window
}
```

A headless tool instead calls `selectWindow(true)` and accepts the floor silently.

## Sharp edges

- **Never null-compare an interface value you got from `select*()`.** Comparing an
  interface value that is actually null against `null` miscompiles today. `selectInput()` /
  `selectAudio()` on an **empty** domain (a hand-built `heap BackendRegistry()` with no
  registration) return a null interface — don't test it with `== null`. Use
  `instance()` (its floor guarantees a non-null result) or call `supports*()`, which the
  registry implements with a boolean-presence scan (`haveBest`) and so is safe even for an
  unregistered domain (it returns `false`, never crashes).
- For the same reason the registry's internal name lookup returns an `int32` index or `-1`
  (`windowIndexByName` et al.), never a possibly-null interface — a pattern to copy if you
  extend it.
- `supports*()` reports against the backend that *would* bind (highest-priority viable, floor
  at worst), not against every registered backend. With only the null floor, every feature is
  `false` (the floor supports nothing).

See `cajeta/ifx/Backend` for the `probe()`/`priority()`/`name()`/`supports(Feature)`
interface each backend implements, and `cajeta/ifx/IfxException` for the loud-failure type.
