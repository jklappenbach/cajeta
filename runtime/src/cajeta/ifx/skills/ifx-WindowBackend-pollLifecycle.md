---
id: ifx-WindowBackend-pollLifecycle
applies-to: [cajeta/ifx/WindowBackend.pollLifecycle]
title: WindowBackend.pollLifecycle — drain mandatory lifecycle / surface-loss events
description: Drains LifecyclePhase transitions (suspend/resume, surface lost/recreated); caller owns and drops the returned array; gfx must rebuild the swapchain on SurfaceRecreated.
---

# `WindowBackend.pollLifecycle(Window w) -> #LifecyclePhase[]`

Drains the window's pending **lifecycle / surface-loss transitions** and hands them to
the caller. Call this every frame alongside `poll` and **act on every phase you read** —
these are MANDATORY contract events, not advisory ones. Mobile *forces* them (Android
`APP_CMD_TERM_WINDOW`, iOS background); the gfx swapchain **MUST** release on
`SurfaceLost` and **rebuild** on `SurfaceRecreated` or mobile breaks (spec §6 / §9.7).

## Return — ownership and the null floor

- Returns `#LifecyclePhase[]`: a **freshly allocated** array, **ownership transferred to
  the caller** (`#`). You own it and drop it; the backend allocates a new batch each poll.
- `null` is a valid return meaning "no transitions this poll." Drain-style, like
  `poll(Window)` (`cajeta/ifx/WindowBackend.poll`).
- The **headless floor never loses a surface, so it yields no events** — `NullWindowBackend`
  returns `null` unconditionally. Code that depends on ever seeing a `SurfaceRecreated`
  will spin forever under the floor; that is correct (there is no surface to lose).
- Elements are `LifecyclePhase` enum values (append-only ordinals):
  `Suspend`(0), `Resume`(1), `SurfaceLost`(2), `SurfaceRecreated`(3). Read them **in order**.

## What it does NOT do

- It does **not** rebuild the swapchain, stop audio, or pause rendering for you — it only
  *reports* the transition. Reacting is the caller's job.
- It does **not** deliver input/resize/close — those come from `poll` as `WindowEvent`s.
  `pollLifecycle` carries only the four `LifecyclePhase` transitions.
- It does **not** block or wait; an empty poll returns `null` immediately.

## Idiomatic example

```cajeta
import cajeta.ifx.BackendRegistry;
import cajeta.ifx.WindowBackend;
import cajeta.ifx.Window;
import cajeta.ifx.LifecyclePhase;

// The registry OWNS the backend; `wb` is a borrowed reference — do not free it.
WindowBackend wb = BackendRegistry.instance().selectWindow(false); // false = interactive
Window w = wb.createWindow("game", 1280, 720);

// Per-frame: drain lifecycle BEFORE rendering so a lost surface is honored this frame.
LifecyclePhase[] phases #= wb.pollLifecycle(w);   // owned by us; dropped at scope end
if (phases != null) {
    int32 i = 0;
    while (i < phases.count()) {
        LifecyclePhase p = phases[i];
        if (p == LifecyclePhase.SurfaceLost)        { /* release the swapchain */ }
        if (p == LifecyclePhase.SurfaceRecreated)   { /* MUST rebuild the swapchain */ }
        if (p == LifecyclePhase.Suspend)            { /* stop rendering + audio */ }
        if (p == LifecyclePhase.Resume)             { /* resume */ }
        i = i + 1;
    }
}
```

## Gotchas

- `selectWindow(false)` (interactive) throws `IfxException` when only the null floor is
  viable — pass `true` to opt into headless, where `pollLifecycle` always returns `null`.
- `w` is the backend-issued `Window` (or `null` under the floor, which ignores it).
- Drain `pollLifecycle` early in the frame; honoring `SurfaceLost`/`SurfaceRecreated`
  before you render avoids presenting to a dead surface.
- Class-level construction/selection detail lives in the `WindowBackend` /
  `BackendRegistry` skills; the phase set lives with `cajeta/ifx/LifecyclePhase`.
