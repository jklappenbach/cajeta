---
id: ifx-input
applies-to: [cajeta/ifx/InputBackend, cajeta/ifx/InputDevice, cajeta/ifx/NullInputBackend]
title: ifx input — gamepad enumeration + polling via the active InputBackend
description: Enumerate gamepads and poll their buttons/axes through the bound InputBackend; InputDevice is the per-device handle, NullInputBackend the empty floor.
---

# ifx input: gamepads through the active backend

This component is **gamepad / extended-input only**, polled. To read input:

1. Get the bound backend: `InputBackend ib = BackendRegistry.instance().selectInput();`
2. Loop `0 .. ib.gamepadCount()`, fetch each `InputDevice` with `ib.gamepad(i)`.
3. Poll state per device: `ib.buttonDown(d, button)` / `ib.axis(d, axis)`.

It does **not** do keyboard or mouse — those arrive as `WindowEvent`s on the window's
event queue (see `cajeta/ifx/WindowEvent`), not here. There is **no rumble / adaptive-
trigger / gyro setter** in v1: `Feature.GamepadRumble`/`AdaptiveTriggers`/`Gyro` are
feature-detection flags only (query `BackendRegistry.supportsInput(feature)`), with no
method on the interface to drive them. There are **no hot-plug events or callbacks** —
re-enumerate each frame. Buttons and axes are **raw `int32` indices**; there is no named
button/axis enum yet.

## Members and roles

| Type | Role | You instantiate? |
|------|------|------------------|
| `InputBackend` | the SPI (extends `Backend`): enumerate + poll gamepads | no — get it from `BackendRegistry.selectInput()` |
| `InputDevice` | lightweight per-gamepad handle (wraps an `int32` index) | rarely — a backend hands it back from `gamepad(i)` |
| `NullInputBackend` | the always-present empty floor: zero devices | no — auto-registered by the registry |

`InputBackend` is **not** something you construct or implement in app code — OS backends
(`cajeta-ifx-*`: XInput / GameInput / evdev / GameController / Paddleboat) implement it
and register through the `BackendRegistry`. ifx itself depends on no backend.

## How they cooperate

`BackendRegistry` (see `cajeta/ifx/BackendRegistry`) owns the registered backends and
binds, for the input domain, the highest-`priority()` viable one — the
`NullInputBackend` floor at worst. **Input has no loud-error case**: an empty device set
is a valid headless outcome, so `selectInput()` never throws (unlike `selectWindow`); it
just returns the best viable backend. With only the floor present, `gamepadCount()` is
`0` and the poll loop runs zero times — headless code "just works."

`InputDevice` carries no behavior beyond `deviceIndex()`; all per-device state lives on
the backend. You pass the device back into `buttonDown`/`axis` — the backend resolves the
actual gamepad from it.

## Worked example (with imports)

```cajeta
import cajeta.ifx.BackendRegistry;
import cajeta.ifx.InputBackend;
import cajeta.ifx.InputDevice;
import cajeta.ifx.Feature;

// The shared registry auto-registers the null floor on first touch, so this is
// safe even with no real backend linked (it returns the empty floor).
InputBackend ib = BackendRegistry.instance().selectInput();

// Enumerate and poll. With only the floor, count() == 0 and the loop is a no-op.
int32 i = 0;
while (i < ib.gamepadCount()) {
    InputDevice d = ib.gamepad(i);
    if (ib.buttonDown(d, 0)) {          // button 0 (raw index)
        float32 lx = ib.axis(d, 0);     // axis 0 (raw index)
        // ... act on lx ...
    }
    i = i + 1;
}

// Optional capabilities are feature-detected against the *bound* backend, not assumed:
boolean rumble = BackendRegistry.instance().supportsInput(Feature.GamepadRumble);
```

Constructing a device directly is legal but only useful for tests/fakes:
`InputDevice d = heap InputDevice(3); d.deviceIndex();  // -> 3`.

## Ownership and lifecycle

- **`selectInput()` returns a borrowed reference.** The `BackendRegistry` owns the
  backend; do not free it or call `close()` (there is none). Re-select rather than cache
  long-term.
- **`gamepad(index)` is nullable.** The `NullInputBackend` (and any backend for an
  out-of-range index) returns `null`. Drive the loop by `gamepadCount()` and only index
  `0 .. count-1`; treat a `null` device as "absent," not an error.
- **`InputDevice` is a transient handle, not a live OS resource.** It holds an index, has
  no destructor obligation, and is invalidated by re-enumeration — fetch it fresh each
  frame rather than retaining across hot-plug.
- **Do not null-compare the `InputBackend` interface value.** Calling
  `BackendRegistry.instance().selectInput()` always yields the auto-registered floor (a
  real object), so you never need to; comparing an interface value to `null` miscompiles
  today (see `BackendRegistry` notes). If you build a registry by hand and register no
  input backend, `selectInput()` *can* return null — register `NullInputBackend` (or use
  `instance()`) to avoid that.

## When to use this vs the window queue

Use this component for **gamepads/controllers**. Use `WindowEvent` off the window's event
queue for **keyboard and mouse** — they come from a different OS API and are not surfaced
here. The two are selected independently by the registry, so a server can bind a real
window with only the empty input floor (and vice versa).
