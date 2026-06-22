---
id: ifx-capability-permission
applies-to: [cajeta/ifx/Feature, cajeta/ifx/Permission, cajeta/ifx/PermissionState]
title: ifx capability detection & permission protocol
description: Feature-detect optional ifx capabilities and request OS-gated permissions without ever crashing — supports*(Feature) plus requestPermission/permissionState returning PermissionState.
---

# ifx capability & permission model

Two separate things ifx programs *must not assume*, both queried at runtime against the
**bound** backend, both designed so the app degrades instead of crashing:

- **Capability** — does the active backend provide an optional `Feature`? Ask
  `BackendRegistry.instance().supportsWindow/Input/Audio(Feature)` → `boolean`. The
  portable floor is always present; everything in `Feature` is feature-detected.
- **Permission** — an OS-gated `Permission` (mic, raw input device). Call
  `requestPermission(Permission)` (may prompt) or `permissionState(Permission)`
  (read-only) on a bound backend → a `PermissionState`. **Neither ever throws** on a
  refusal; denial is the returned `Denied`.

If you are choosing/binding a backend rather than interrogating one, that is
`cajeta/ifx/BackendRegistry` — this skill is only the capability/permission layer on top.

## Members & roles

- **`Feature`** — enum (int32-aliased ordinal), 14 optional capabilities:
  `MultiWindow, WindowPositioning, CursorWarp, PointerLock, KeyboardMouse, Touch,
  GamepadRumble, AdaptiveTriggers, Gyro, VirtualGamepad, AudioCapture, LoopbackCapture,
  ExclusiveAudio, Hdr`. The ordinal is the contract a backend reports against —
  **append-only, never reorder**.
- **`Permission`** — enum: `Microphone` (0), `InputDevice` (1). Append-only.
- **`PermissionState`** — enum, the result: `NotRequired` (0, platform doesn't gate it),
  `Pending` (1, async OS prompt outstanding), `Granted` (2), `Denied` (3).

All three are plain value enums — passed and returned **by value**, no `#` transfer, no
ownership, no null. The behavior lives on the `Backend` interface
(`supports`/`requestPermission`/`permissionState`); `BackendRegistry` wraps `supports`
as the `supports{Window,Input,Audio}` convenience that targets the bound backend.

## Collaboration / call sequence

`Feature`/`Permission` are *inputs*; `boolean`/`PermissionState` are *outputs*. The
registry owns the backends; you query through it.

1. `Backend.supports(Feature)` → does this one backend provide it.
2. `BackendRegistry.supportsAudio(Feature)` etc. scans for the highest-`priority()`
   viable backend in that domain and forwards `supports` to it. With **only the null
   floor** present (or **no** backend registered for the domain), the answer is `false`
   for every feature — the floor supports nothing.
3. To act on a permission-gated path: gate on `Granted`, degrade on `Denied`, and treat
   `Pending`/`NotRequired` per platform (below).

## What it does NOT do

- `supports*` / `permissionState` never prompt and never throw. `requestPermission` may
  prompt but still **never throws on denial** (contrast `selectWindow`, which *does*
  throw `IfxException` when an interactive window has no real backend — that is binding,
  not capability).
- There is no `Feature`/`Permission` → string name or count helper here; do not look for
  reflection. The ordinals are the stable contract.
- `Granted`/`Denied` are not booleans — handle all four `PermissionState` values
  (`Pending` and `NotRequired` are real outcomes, not errors).

## Worked example

```cajeta
import cajeta.ifx.BackendRegistry;
import cajeta.ifx.AudioBackend;
import cajeta.ifx.Feature;
import cajeta.ifx.Permission;
import cajeta.ifx.PermissionState;

BackendRegistry ifx = BackendRegistry.instance();

// Capability: only open a capture path if the active audio backend can do it.
if (ifx.supportsAudio(Feature.AudioCapture)) {
    AudioBackend audio = ifx.selectAudio();   // bound backend; registry-owned — do NOT free

    // Permission: request, then branch on every state — denial is reported, not thrown.
    PermissionState state = audio.requestPermission(Permission.Microphone);
    if (state == PermissionState.Granted || state == PermissionState.NotRequired) {
        // start capture
    } else if (state == PermissionState.Pending) {
        // async OS prompt outstanding — poll audio.permissionState(Permission.Microphone) later
    } else {
        // Denied — degrade (run without mic), never crash
    }
} else {
    // no capture on this platform/backend — take the silent/degraded path
}
```

## Ownership & lifecycle

- The `Feature`/`Permission` arguments and the returned `boolean`/`PermissionState` are
  value-copied; nothing to free, nothing nullable.
- `select*()` / `instance()` hand back **registry-owned, borrowed** references — the
  registry retains them for the process lifetime; do not free or store past the registry.
- `permissionState` is the read-only sibling of `requestPermission`: poll it to observe a
  `Pending` request resolving without re-prompting.
