---
id: ifx-window
applies-to: [cajeta/ifx/WindowBackend, cajeta/ifx/Window, cajeta/ifx/Surface, cajeta/ifx/WindowEvent, cajeta/ifx/LifecyclePhase, cajeta/ifx/NullWindowBackend]
title: ifx Window domain — backend, window, surface, event/lifecycle draining
description: How WindowBackend creates a Window, hands the gfx swapchain its opaque Surface, and drains owned WindowEvent/LifecyclePhase batches (NullWindowBackend = headless floor).
---

# ifx Window domain

The window domain is one SPI plus four value types. `WindowBackend` (a `Backend`) is the
only entry point you call: it creates a `Window`, yields the opaque `Surface` the gfx
swapchain consumes, and drains two per-poll batches — input `WindowEvent[]` and mandatory
`LifecyclePhase[]`. `Window`, `Surface`, `WindowEvent` are plain final value types you
receive, never instantiate yourself; `LifecyclePhase` is an enum. `NullWindowBackend` is
the always-present headless floor that creates nothing and drains nothing.

You do **not** get a `WindowBackend` by constructing one — resolve it from the registry
(see `cajeta/ifx/BackendRegistry`): `BackendRegistry.instance().selectWindow(headless)`.
Asking for an interactive window when only the null floor is viable throws `IfxException`;
`selectWindow(true)` (headless) binds the floor silently.

## Members and roles

- **`WindowBackend`** (interface, extends `Backend`) — the per-OS provider (Win32 /
  Wayland+X11 / Cocoa / UIKit / ANativeWindow / harness). The thing you call.
- **`Window`** (`final class`) — portable window handle. One field, `int64 backendHandle`;
  `handle()` returns the opaque per-backend value. On mobile a "window" is the single
  fullscreen surface.
- **`Surface`** (`final class`) — the opaque present target you hand to the gfx swapchain.
  Wraps a platform handle (HWND / wl_surface / CAMetalLayer / ANativeWindow) you never
  inspect. `isHeadless()` (true when `nativeHandle == 0`), `surfaceWidth()`,
  `surfaceHeight()`.
- **`WindowEvent`** (`final class`) — one portable input/window event. `eventType()`
  returns the tag: `0 NONE · 1 KEY_DOWN · 2 KEY_UP · 3 MOUSE_MOVE · 4 MOUSE_BTN ·
  5 RESIZE · 6 CLOSE`. Other fields: `code`, `x`, `y`, `width`, `height`.
- **`LifecyclePhase`** (enum, append-only ordinals) — `Suspend(0)`, `Resume(1)`,
  `SurfaceLost(2)`, `SurfaceRecreated(3)`.
- **`NullWindowBackend`** (`final class implements WindowBackend`) — headless floor,
  priority `-1000`, `name() == "null"`, `supports()` always false.

## Call sequence (per backend)

```
WindowBackend wb = BackendRegistry.instance().selectWindow(false);  // borrowed; registry owns it
Window  w = wb.createWindow(title, width, height);  // backend owns the OS window
Surface s = wb.surfaceOf(w);                         // hand s to the gfx swapchain
// frame loop:
#WindowEvent[]   events = wb.poll(w);          // owned by you — drop after draining
#LifecyclePhase[] phases = wb.pollLifecycle(w); // owned by you — drop after draining
// ...
wb.destroy(w);   // release the OS window when done
```

## Ownership / lifecycle (the part that bites)

- **`poll(w)` and `pollLifecycle(w)` return `#`-transferred arrays** — each is freshly
  allocated per call and ownership passes to you; you drop them. The backend hands over a
  new batch every poll. A `null` return means "no events this poll" and is a valid `#`
  result — guard for it before iterating.
- **`Window` and `Surface` are owned by the backend**, not by you. Do not free them; call
  `wb.destroy(w)` to release the OS window. `surfaceOf` returns a handle the gfx swapchain
  reads — don't outlive the window.
- **`WindowEvent` values live inside the polled array** — once you drop the array they are
  gone; copy out any field you need to keep.
- The methods on `Backend` (`probe`, `priority`, `name`, `supports`, `requestPermission`,
  `permissionState`) are inherited; permission denials return `PermissionState.Denied`,
  never throw.

## LifecyclePhase is mandatory, not optional

`pollLifecycle` is a contract obligation, not a convenience. Mobile forces these (Android
`APP_CMD_TERM_WINDOW`, iOS background); desktop emits the same shape (minimize/restore).
The gfx swapchain **MUST** release on `SurfaceLost` and rebuild on `SurfaceRecreated` —
skip it and mobile breaks. Drain `pollLifecycle` every frame alongside `poll`. The
headless floor never loses a surface, so it yields none (returns `null`).

## NullWindowBackend — what it does NOT do

The floor is the headless guarantee: any program that imports ifx runs with no OS backend
linked. It creates **no** window (`createWindow` returns `null`), returns a headless
`Surface` from `surfaceOf` (actually `null` here — gfx then renders to an offscreen
image), and drains nothing (`poll`/`pollLifecycle` return `null`). Choosing headless is
fine; *needing* a real window with only the floor present is the loud-error case the
registry's `selectWindow(false)` raises as `IfxException` — it never hands back a silent
black screen.

## Worked example (headless drain loop)

```cajeta
import cajeta.ifx.BackendRegistry;
import cajeta.ifx.WindowBackend;
import cajeta.ifx.Window;
import cajeta.ifx.Surface;
import cajeta.ifx.WindowEvent;
import cajeta.ifx.LifecyclePhase;
import cajeta.lang.String;

WindowBackend wb = BackendRegistry.instance().selectWindow(true);   // headless: floor is fine
Window  w = wb.createWindow("demo", 1280u, 720u);
Surface s = wb.surfaceOf(w);
boolean running = true;
while (running) {
    #WindowEvent[] events = wb.poll(w);          // OWNED — null means no events
    if (events != null) {
        int32 i = 0;
        while (i < events.length) {
            if (events[i].eventType() == 6) { running = false; }   // 6 == CLOSE
            i = i + 1;
        }
    }                                            // events dropped here

    #LifecyclePhase[] phases = wb.pollLifecycle(w);   // OWNED — rebuild swapchain on SurfaceRecreated
    // ... hand `phases` to the gfx swapchain ...
}
wb.destroy(w);
```

## See also

- `cajeta/ifx/BackendRegistry` — how `WindowBackend` is selected (probe/priority,
  three-cases policy, `CAJETA_IFX_WINDOW` override) and why `selectWindow` can throw.
- `cajeta/ifx/Backend` — the shared probe/priority/permission base.
