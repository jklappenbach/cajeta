---
id: ifx-overview
applies-to: [cajeta.ifx]
title: cajeta.ifx — window/input/audio service-provider framework (orientation & routing)
description: Routing map for ifx — registry+probe+priority backend selection, the always-present null floor, CAJETA_IFX_* overrides; start at BackendRegistry/IfxInfo.
---

# cajeta.ifx — orientation & routing

`cajeta.ifx` is the **portable contract** for window/surface/event, extended input
(gamepads), and audio. It owns no OS code and links no backend (**dependency inversion,
no FFI**): OS backends (`cajeta-ifx-<os>`) and the capture/replay `cajeta-ifx-harness`
implement these interfaces and register at load through `BackendRegistry`. At launch the
registry binds, **per domain independently**, the highest-`priority()` backend whose
`probe()` is viable. An always-present **null floor** guarantees selection never comes up
empty. **Start at `BackendRegistry` (selection) or `IfxInfo` (the app-facing facade).**

If your task is *implementing* an OS backend, you implement the `*Backend` interfaces
here but the OS code lives in an external library — this package is the contract only.

## Task → entry point

| Want to… | Start with |
| --- | --- |
| Get the process-wide registry (floor auto-registered) | `BackendRegistry.instance()` |
| Register an OS backend at load | `BackendRegistry.instance().registerWindow/registerInput/registerAudio(#backend)` |
| Bind the window backend for this launch | `registry.selectWindow(boolean headless)` |
| Bind input / audio backend | `registry.selectInput()` / `registry.selectAudio()` |
| Snapshot which backends are bound (never throws) | `IfxInfo.describe()` → `windowBackendName()` etc. |
| Feature-detect an optional capability on the bound stack | `IfxInfo.supportsWindow/Input/Audio(Feature)` |
| Request / read an OS permission (mic, raw input) | `Backend.requestPermission(Permission)` / `permissionState(Permission)` → `PermissionState` |
| Create a window / drain events | `WindowBackend.createWindow` / `poll` / `pollLifecycle` (via the bound backend) |
| Open audio output / submit PCM | `AudioBackend.openOutput` / `submit` / `close` |
| Enumerate gamepads / read buttons & axes | `InputBackend.gamepadCount` / `gamepad` / `buttonDown` / `axis` |
| Record presented frames / audio mix (recording seam) | `VideoSink` / `AudioSink` — fallbacks `PngSequenceVideoSink` / `WavAudioSink` |

Negative rows (avoid the dead end):
- **Keyboard & mouse are NOT in `InputBackend`** — they arrive as `WindowEvent`s through
  `WindowBackend.poll`. `InputBackend` is gamepads only.
- **No real PNG/WAV/video encoding lives in stdlib.** `PngSequenceVideoSink` /
  `WavAudioSink` only record params and count frames/buffers; real byte-encoding ships in
  `cajeta-ifx-harness`. No licensed codec ships in stdlib.
- **No window is created by `NullWindowBackend`** — it returns a headless `Surface`
  (`nativeHandle == 0`); gfx renders offscreen. Needing a real window with only the floor
  present is the loud-error case below.

## Cross-cutting invariants

- **Selection = registry + `probe()` + `priority()`.** Highest priority among viable
  wins; **on a priority tie the first-registered wins** (loop replaces only on strictly
  greater). The floor's `priority()` is `-1000`, so any real backend outranks it.
- **Always-present null floor.** `BackendRegistry.instance()` lazily auto-registers
  `NullWindowBackend` / `NullInputBackend` / `NullAudioBackend` on first touch (all
  `name() == "null"`, `probe() == true`, `supports(...) == false`). A raw
  `heap BackendRegistry()` is **empty** — only `instance()` carries the floor.
- **Window has a loud-error case; input/audio do not.** `selectWindow(headless)`:
  headless `true` binds the floor silently; headless `false` with only the floor viable
  **throws `IfxException`** rather than hand back a silent black screen. `selectInput()`/
  `selectAudio()` always return the best viable (floor at worst) — empty input / silent
  audio is a valid headless outcome.
- **`CAJETA_IFX_WINDOW` / `_INPUT` / `_AUDIO` env overrides ARE wired** (read via
  `System.env.get`). A set value forces the registered backend of that `name()` over
  probe/priority, and **bypasses the headless policy** (forcing `"null"` is an explicit
  opt-in). An **unknown name fails loudly with `IfxException`**, even for headless.
  (The class-doc claim that overrides are "not wired yet" is stale — the code reads env.)
- **Ownership.** Drained-array returns are `#` ownership-transfer — the caller drops them:
  `WindowBackend.poll` → `#WindowEvent[]`, `pollLifecycle` → `#LifecyclePhase[]`. `null`
  (no events) is a valid `#` return. `IfxInfo.describe()` returns a `#IfxInfo` (caller
  owns). `register*` takes `#` ownership of the backend. Sink buffers (`writeFrame`/
  `writeSamples`) and `AudioBackend.submit` frames are **borrowed** — copy to keep.
- **Errors.** `IfxException extends RecoverableException` (catchable/testable; left
  uncaught it aborts loudly at top level). Denied permissions are **reported as
  `PermissionState.Denied`, never thrown** — the app degrades. Backends never throw for a
  missing device; they return empty/silent.
- **Eager stdlib package.** `cajeta.ifx` is prescanned/parsed at compiler startup even
  with no import (only `cajeta.math` is lazy). Importing `IfxInfo` is enough to anchor it.
- **Interface-value null caution (codegen).** The registry tracks "found one?" with a
  boolean and uses `name()`/index lookups rather than null-comparing an interface value,
  because comparing a null interface value against `null` miscompiles today. If you write
  registry-style code, prefer the same boolean-presence shape.

## Canonical end-to-end example

```cajeta
import cajeta.ifx.BackendRegistry;
import cajeta.ifx.WindowBackend;
import cajeta.ifx.IfxInfo;
import cajeta.ifx.Feature;
import cajeta.lang.String;

// Backends register themselves at load; the floor is auto-registered by instance().
BackendRegistry registry = BackendRegistry.instance();

// Bind the window domain. Pass headless=true to accept the null floor offscreen;
// headless=false throws IfxException if only the floor is viable.
WindowBackend window = registry.selectWindow(true);

// What actually got bound this launch? describe() never throws (headless-tolerant).
IfxInfo info = IfxInfo.describe();      // #IfxInfo — caller owns
String bound = info.windowBackendName();      // "win32" | "wayland" | ... | "null"

// Program to the floor; feature-detect the rest against the *bound* stack.
boolean hdr = IfxInfo.supportsWindow(Feature.Hdr);   // false on the null floor
```

## Disambiguation

- **`BackendRegistry` vs `IfxInfo`.** `BackendRegistry` is the mechanism (register +
  select + per-launch bind, and the loud-error path). `IfxInfo` is the read-only facade —
  `describe()` snapshots names, `supports*` feature-detects; it never throws and is what
  app code should use to ask "what am I running on / what's available?".
- **`selectWindow(true)` vs `selectWindow(false)`.** `true` = headless-tolerant (servers/
  CI/harness, accepts the floor); `false` = interactive (must get a real window, else
  loud). `IfxInfo.describe()` always uses the headless form so describing can't throw.
- **`WindowBackend.poll` vs `InputBackend`.** Keyboard/mouse/resize/close = `poll`
  (`WindowEvent`); gamepads = `InputBackend`. They are separate OS APIs and bound
  separately.
- **`Feature` (optional capability) vs `LifecyclePhase` (mandatory).** `Feature` is
  feature-detected and may be absent. `LifecyclePhase` (suspend/resume, surface
  lost/recreated from `pollLifecycle`) is **non-optional contract** — mobile forces it.

## Setup / preconditions

Pure-Cajeta stdlib package, no extra dependency or capability to use the contract. To get
a **real** window/input/audio you must link the matching `cajeta-ifx-<os>` backend (and
`cajeta-ifx-harness` for capture/replay and the real sink encoders); with none linked the
program runs on the null floor. v1 statically links bundled backends and dispatches at
launch.

## Downward pointers

For per-type detail read the class skills (when present) for `BackendRegistry`, `Backend`
and the per-domain `WindowBackend`/`InputBackend`/`AudioBackend`, `IfxInfo`, the value
types (`Window`, `Surface`, `WindowEvent`, `InputDevice`, `AudioStream`), the enums
(`Feature`, `LifecyclePhase`, `Permission`, `PermissionState`), `IfxException`, and the
recording seam (`VideoSink`/`AudioSink` + `PngSequenceVideoSink`/`WavAudioSink`). Design
rationale: `documents/cajeta-gfx/cajeta-gfx-spec.md` §9.
