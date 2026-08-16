---
id: ifx-backend-selection
applies-to: [cajeta/ifx/Backend, cajeta/ifx/BackendRegistry, cajeta/ifx/IfxInfo]
title: ifx backend self-registration and per-domain selection
description: How ifx backends self-register into the shared BackendRegistry and how it binds the highest-priority viable backend per domain, with IfxInfo as the app-facing snapshot.
---

# ifx backend selection — the SPI keystone

To **find out / pick what's bound**, go through `BackendRegistry`; to get an app-facing
**snapshot or feature-detect**, use `IfxInfo`. You do **not** implement `Backend`
yourself unless you are writing an OS backend — `cajeta.ifx` is a portable contract with
no FFI, and the bundled `Null*` floor is auto-registered, so an app just selects.

Three cooperating roles:

- **`Backend`** — the base SPI interface (`probe()`, `priority()`, `name()`,
  `supports(Feature)`, plus permission gating). The three domain interfaces
  `WindowBackend` / `InputBackend` / `AudioBackend` extend it; OS libraries
  (`cajeta-ifx-*`) and the harness implement those. Selection reads exactly two methods:
  `probe()` (viable here?) and `priority()` (weight; higher wins).
- **`BackendRegistry`** — registry + probe + dispatcher. Holds three **independent**
  per-domain lists in registration order, and binds, per domain, the highest-`priority()`
  backend whose `probe()` is true.
- **`IfxInfo`** — app-facing facade. `describe()` returns a `#IfxInfo` snapshot of the
  bound backend `name()` per domain; static `supports*` delegate to the registry.

## Object graph and the binding rule

`BackendRegistry.instance()` is the process-wide shared registry. On first touch it
auto-registers the always-present floor (`NullWindowBackend` / `NullInputBackend` /
`NullAudioBackend`, each `name()=="null"`, `priority()==-1000`), so selection is never
empty. Backends register into the **same instance** at load:

```
BackendRegistry.instance().registerWindow(heap Win32WindowBackend());
```

Because the floor registers first and is lowest priority, any real backend that registers
later wins; if none ever registers, selection falls to the floor.

The select scan binds the **highest `priority()` among `probe()==true`** candidates.
**Tie-break is deterministic: first-registered wins** — the loop replaces the incumbent
only on a *strictly greater* priority. A `probe()==false` backend is never bound, even at
higher priority.

Window has a special **three-cases policy** (`selectWindow(boolean headless)`):
1. a real viable backend → bind it;
2. within-OS choice among real backends → `probe()`/`priority()` (ties: first wins);
3. only the null floor viable → `headless==true` binds null silently; `headless==false`
   (interactive) throws `IfxException` rather than hand back a silent black screen.

Input and audio have **no loud case**: an empty device set / silent output is a valid
headless outcome, so `selectInput()` / `selectAudio()` just return the best viable
backend (the floor at worst). With a domain that has no backend registered at all, the
`select*` value can be a null interface — the registry's own `supports*` guard this with a
boolean-presence flag rather than null-comparing the interface value.

## Ownership and lifecycle

- Registered backends are **borrowed by the registry** — `register*` adds the `heap`
  instance to a list; you transfer it in with `heap` and do not free it (process-lifetime).
- `select*` returns a **borrowed** backend reference owned by the registry — do not free
  it; it stays valid for the process.
- `IfxInfo.describe()` returns `#IfxInfo` — **ownership transfers to the caller**, who
  drops it. Its `name()` strings are snapshots taken at the describe() call.
- `Backend.requestPermission` / `permissionState` return a `PermissionState` enum;
  a denial is `Denied` (returned, **never thrown**) — the app degrades, not crashes.

## When to use which

- Need the actual bound backend object to drive it → `registry.selectWindow(...)` /
  `selectInput()` / `selectAudio()`.
- Just need to report or branch on what's bound → `IfxInfo.describe()` (headless-tolerant:
  it uses the headless window bind, so it never throws even on a floor-only setup).
- Need optional-capability detection → `IfxInfo.supportsWindow/Input/Audio(Feature)`
  (facade) or the registry's `supports*` (same result). Floor supports nothing → every
  optional `Feature` is false.

## What this does NOT do

- No FFI / OS code lives here — backends are external. There is no `createWindow` on the
  registry; you get a `WindowBackend` and call it.
- The `CAJETA_IFX_WINDOW/INPUT/AUDIO` env override is *coded* in `select*` (forces a
  registered backend by `name()`, unknown name → `IfxException`) but depends on a
  pure-Cajeta env primitive; v1 statically links bundled backends and dispatches at launch.
- No automatic GPU/swapchain wiring — `WindowBackend.surfaceOf` yields the `Surface` gfx
  consumes, but that's a separate component.

## Worked example

```cajeta
package app;

import cajeta.ifx.BackendRegistry;
import cajeta.ifx.WindowBackend;
import cajeta.ifx.IfxInfo;
import cajeta.ifx.Feature;
import cajeta.ifx.IfxException;
import cajeta.lang.String;

public final class Launch {
    public static int32 run() {
        BackendRegistry registry = BackendRegistry.instance();   // floor auto-registered

        // Interactive window: throws IfxException if only the null floor is viable.
        WindowBackend window = registry.selectWindow(false);     // borrowed; do not free

        // Feature-detect against the bound stack (floor -> false).
        if (IfxInfo.supportsWindow(Feature.MultiWindow)) {
            // ... multi-window path
        }

        // Snapshot what got bound (headless-tolerant, never throws).
        IfxInfo info #= IfxInfo.describe();
        String boundName = info.windowBackendName();             // e.g. "win32" | "null"
        return IfxInfo.version();
    }
}
```

For the loud/headless contract see `cajeta/ifx/IfxException`; for the optional-capability
ordinals see `cajeta/ifx/Feature`; for the floor's behavior see the `Null*Backend` types.
