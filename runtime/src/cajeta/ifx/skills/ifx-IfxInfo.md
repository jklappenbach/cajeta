---
id: ifx-IfxInfo
applies-to: [cajeta/ifx/IfxInfo]
title: IfxInfo — app-facing ifx facade + bound-backend snapshot
description: Static facade for cajeta.ifx (version, capability feature-detection, headless-tolerant describe) plus the per-domain backendName accessors of the snapshot value it returns.
---

# IfxInfo

**Access point — this is where an app touches `cajeta.ifx`.** Two faces in one type:

1. **Static facade** — `version()`, `supportsWindow/Input/Audio(Feature)`, and
   `describe()`. Call these directly; never instantiate.
2. **Snapshot value** — an `IfxInfo` instance, obtained *only* from `describe()`, that
   records which backend `ifx` bound for each domain (window / input / audio) this launch.
   Its three `*BackendName()` getters read that snapshot back.

For *what the package is and how backends get bound*, see the package skill
(`cajeta/ifx`) and `cajeta/ifx/BackendRegistry`. This skill is just the facade/value.

## Construction & ownership

You do **not** construct `IfxInfo` — the constructor is private. Receive one from
`describe()`:

```cajeta
import cajeta.ifx.IfxInfo;

IfxInfo info #= IfxInfo.describe();   // owned heap value, transferred to you
```

`describe()` returns `#IfxInfo` — an **owned** heap value the caller owns and drops on
scope. There is **no** `close()`/dispose; it is a plain value, not a handle.

## The methods that matter

- `static int32 version()` — facade-surface revision constant (currently `1`). This is
  **not** a backend or OS version.
- `static #IfxInfo describe()` — snapshot the active per-domain bind. **Never null,
  never throws** (see below).
- `String windowBackendName()` / `inputBackendName()` / `audioBackendName()` — the
  bound backend's `name()` for each domain in this snapshot, e.g. `"win32"` or the
  floor `"null"`. Each is a **borrowed** view into the snapshot — copy it if you need
  it to outlive the `IfxInfo`.
- `static boolean supportsWindow(Feature f)` / `supportsInput(Feature f)` /
  `supportsAudio(Feature f)` — does the *bound* backend for that domain provide the
  optional `Feature`? (See `cajeta/ifx/Feature` for the enum.) With no OS backend
  linked, only the `"null"` floor is bound and **every optional feature is `false`**.

## Headless-tolerant — describe() never throws

`describe()` snapshots the **headless** window bind (`selectWindow(true)` internally),
so on a floor-only / offscreen setup it reports the `"null"` floor name instead of
raising the interactive loud-error. Reporting what is bound must never crash. Each
domain always resolves to a real backend or the floor — never null — because the null
floor is auto-registered on first registry access.

The `supports*()` queries likewise never throw; an unsupported or unregistered domain
is simply `false`.

## What it does NOT do

- It does **not** create windows, open audio, or poll input — those live on the
  per-domain backends (`WindowBackend` / `InputBackend` / `AudioBackend`).
- It is **not** the registry. Registration and the bind policy belong to
  `cajeta/ifx/BackendRegistry`; `IfxInfo` is a thin static pass-through to
  `BackendRegistry.instance()`.
- It never raises `IfxException`. The interactive "no viable window backend" loud-error
  is thrown by `BackendRegistry.selectWindow(false)` — the real-window request path —
  **not** by anything on `IfxInfo`.
- There are no setters and no second constructor; the snapshot is immutable and
  describe-only.

## Worked example (mirrors IfxFacadeTests)

```cajeta
import cajeta.ifx.IfxInfo;
import cajeta.ifx.Feature;

public final class D {
    public static int32 run() {
        int32 v = IfxInfo.version();                      // 1

        // Feature-detect against the *bound* backend; floor-only → false.
        if (IfxInfo.supportsWindow(Feature.MultiWindow)) { /* desktop path */ }

        // Snapshot the active bind (headless-tolerant: never throws).
        IfxInfo info #= IfxInfo.describe();
        if (info.windowBackendName().equals("null")) {
            // running on the floor — no OS ifx backend linked
        }
        return v;
    }
}
```
