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

- `register*(backend)` is **DUAL-ROLE**, the `ArrayList` contract exactly. It declares a
  plain interface formal (`WindowBackend backend`) and forwards it to `ArrayList.add`, which
  stores with `#=` — and §2.3's rule is that `#=` records the SOURCE's mode. A plain formal
  does not mean "always a borrow": its mode arrives at run time in the call's transfer word,
  so what the registry records is what the CALL SITE sent.
- **Surrender it and the registry owns it, for the life of the process:**
  `r.registerWindow(heap Win32WindowBackend())` or `r.registerWindow(#win32)`. Nothing else
  has to stay alive. This is what `instance()` itself does for the three null-floor
  backends, which is also the proof the registry must support it — those are built inside
  `instance()` and there is no other binding anywhere that could own them.
- **Lend it and you keep the title:** `r.registerWindow(win32)`. Then `win32` must outlive
  every `select*`/`supports*` call, because the registry is holding your pointer.
- `select*()` and `supports*()` return a **borrowed** reference either way. Do **not** free
  it; do not use it past the lifetime of whoever owns it (the registry when surrendered,
  your binding when lent).

## Example (idiomatic; the registry surface exercised by test/ifx/IfxRegistryTests.cpp)

```cajeta
import cajeta.ifx.BackendRegistry;
import cajeta.ifx.WindowBackend;
import cajeta.ifx.Feature;
import cajeta.ifx.IfxException;

// An OS backend registers itself at load (the null floor is already there via instance()).
// Surrender it: the registry owns it for the life of the process, and nothing else has to
// stay alive. Lend instead (`registerWindow(win32)`) only if you want to keep the title,
// and then `win32` must outlive every select*/supports* call.
BackendRegistry.instance().registerWindow(heap Win32WindowBackend());

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
