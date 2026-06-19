# ifx — interface framework (facade contract, reference scaffold)

`ifx` is the **stdlib facade** for window / input / audio: one portable contract apps and engines
compile against, with per-OS backends and a test harness implementing it from *outside* stdlib.
Full design: `documents/cajeta-gfx/cajeta-gfx-spec.md` §9.

> **Status: reference scaffold, NOT wired into the build.** stdlib is glob-embedded into the
> compiler (`cmake/EmbedStdlib.cmake`), so the contract lives here for review during the spec
> phase. Its **target home is `runtime/src/cajeta/ifx/`** (package `cajeta.ifx`); it moves there
> when the plan executes and it compiles + tests. Final stdlib layout = one public type per file.

## The stdlib half (this package, `cajeta.ifx`)
- `IfxInfo` — package anchor (on-demand parse) + surface version.
- `Surface` — opaque present handle the gfx swapchain consumes (+ drawable size & scale).
- `Window`/`WindowEvent`, `AudioStream`, `InputDevice` — the three portable domain contracts.
- `Feature` + `supports(Feature)` — the **capability query** (multi-window, pointer-lock vs warp,
  touch, rumble, loopback capture, exclusive audio, …); apps program to the portable floor and
  feature-detect the rest (spec §9.7).
- **Lifecycle events** (suspend/resume, **surface-lost/surface-recreated**) + **permission state**
  (mic capture, input-device access) — mandatory contract surface, because mobile forces them.
- `Backend` SPI (`WindowBackend`/`AudioBackend`/`InputBackend`) + `BackendRegistry` (registry +
  probe + dispatch + env override) + `NullBackend` (the silent headless floor, always present).
- `VideoSink` — codec SPI; PNG/WAV fallback in-tree, real codecs are optional providers.

The **binding** of each domain differs by OS — flat C FFI (Windows/Linux/Android NDK), an Obj-C
shim (macOS/iOS), or a JNI airlock (Android text/permission). `ifx` stays pure; the shim/JNI lives
in each backend repo. Full feature matrix + gap plan: spec §9.7.

This scaffold ships the contract anchor (`IfxInfo`) and the SPI (`Backend`) so the external repos
have a concrete seam to implement; the remaining types are specified in §9 and land with the plan.

## The external, optional half (separate repos)
| Repo | Kind | Role |
|---|---|---|
| `cajeta-ifx-backend` | melt | required selector — maps build target → backend(s) |
| `cajeta-ifx-windows` | library | Win32 / WASAPI / Raw Input backend |
| `cajeta-ifx-linux` | library | Wayland+X11 / PipeWire+ALSA / libinput backend |
| `cajeta-ifx-macos` | library | Cocoa / CoreAudio / GameController backend |
| `cajeta-ifx-ios` | library | UIKit / CoreAudio / GameController backend |
| `cajeta-ifx-android` | library | NDK ANativeWindow / AAudio / AInput backend |
| `cajeta-ifx-harness` | library | capture/replay testing backend (no FFI) |

Each backend **implements the `Backend` SPI and registers** through `BackendRegistry`; `ifx`
depends on no backend (dependency inversion).
