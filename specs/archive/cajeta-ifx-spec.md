# cajeta-ifx — Spec

**Status:** Draft. Requirements for `cajeta.ifx` ("interface framework") — the stdlib
**facade + backend SPI + registry** for the window / input / audio platform layer.
Extracted from `gfx-spec.md` §9 (Platform layer); that section remains the
umbrella rationale.

## 1. Definition

`ifx` is the **portable, pure-Cajeta, no-FFI contract** for the three platform-I/O
domains — **window**, **input**, **audio** — plus the **registry/dispatch** that binds a
concrete backend at launch and the **null/headless floor** that lets any program using
`ifx` compile and run with no OS backend present.

- **It is stdlib.** Built into the toolchain, never declared, never fetched (like
  `cajeta.lang` / `cajeta.io`). Every app compiles against it for free.
- **It carries no OS code.** Window/input/audio are OS services reached through the C
  ABI — a *runtime HAL*, not an LLVM codegen target (§9.1). So `ifx` defines only the
  contract + SPI + registry; the OS bindings live in external, vendor-versioned
  backends (`cajeta-ifx-*`) and the test `harness`.
- **Dependency inversion.** Backends depend on `ifx`; `ifx` depends on no backend. A
  backend FFI-binds its OS, implements an `ifx` SPI interface, and **registers itself**
  at load. `ifx` selects among the registered backends by `probe()` + `priority()`.
- **`Surface` is opaque.** `HWND` / `wl_surface` / `CAMetalLayer` are never inspected by
  `ifx`; the `cajeta.xpu.gfx` swapchain pairs the `Surface` with the matching Vulkan WSI
  extension. The Surface→present path is owned by gfx, not `ifx`.

### Scope
The stdlib package `cajeta.ifx`: contract types, the backend SPI, `BackendRegistry` +
dispatch, the null floor, the capability/permission/lifecycle model, and the recording
**SPI seam** that the harness and codec providers register into.

### Non-goals
- **No OS code / FFI in `ifx`** — that lives in the per-OS backend repos.
- **No GPU / WSI** — the swapchain and WSI-extension pairing belong to `cajeta.xpu.gfx`.
- **No backend selection at build time** — that is the `cajeta-ifx-backend` melt (BOM).
- **No codec implementations** — only the SPI seam + the royalty-free PNG/WAV fallback
  contract; real codecs are optional external providers (§9.6).

---

## 2. Portable contract (the write-once floor)

The API every app programs against; identical on every platform (§9.2). Apps target
**the floor** and feature-detect the rest (§5).

**Requirements**
- `Window`, `Surface`, `WindowEvent` — window/surface lifecycle + a polled event stream.
- `InputDevice` — keyboard/pointer **or** touch, plus gamepad enumerate + buttons/axes.
- `AudioStream` — output, and permission-gated capture.
- `IfxInfo` — the bound backend's name + active capability/permission state.
- The floor every backend MUST provide: one surface (+ drawable size & scale), a present
  hook, lifecycle events, key/pointer **or** touch input, gamepad enumerate, audio
  output + capture (permission-gated), and the capability query (§9.7).

**Use cases**
- 2.1 — As an app, when I call `createWindow(title, w, h)`, then I receive a `Window`
  whose `Surface` the gfx swapchain can present to, with the requested extent.
- 2.2 — As an app, when I `poll(window)`, then I receive the `WindowEvent`s that
  occurred since the last poll (key/pointer/touch/resize/lifecycle), in order.
- 2.3 — As an app, when I open an `AudioStream` for output, then a known PCM buffer is
  rendered to the device; for capture, I read device PCM (subject to §6 permission).
- 2.4 — As an app, when I enumerate `InputDevice`s, then connected gamepads appear with
  buttons/axes regardless of OS gamepad API.

---

## 3. Backend SPI

The service-provider interface backends implement (§9.3). One base + three domain
interfaces, kept separate because their use cases and vendor APIs are independent (a
server wants `audio` with no `window`; gamepads come from a different OS API than the
window queue).

**Requirements**
- `Backend` (base): `probe() → boolean`, `priority() → int32`, `name() → String`,
  `supports(Feature) → boolean`.
- `WindowBackend extends Backend`: `createWindow`, `surfaceOf`, `poll`, `destroy`.
- `InputBackend extends Backend`: device enumeration + state/events.
- `AudioBackend extends Backend`: output + capture stream creation.
- The three domains are independently registrable and independently selectable.

**Use cases**
- 3.1 — As a backend author, when my library loads, then I register one or more domain
  backends with `ifx` and need no change to `ifx` itself (dependency inversion).
- 3.2 — As a backend, when `probe()` is called, then I report whether I am viable in the
  current environment (e.g. `WAYLAND_DISPLAY` set → the Wayland backend is viable).
- 3.3 — As a backend, when asked `supports(feature)`, then I answer per my active stack
  (e.g. programmatic window position is `false` on Wayland, `true` on X11).

---

## 4. Registry & dispatch — `BackendRegistry`

The keystone: a **registry + probe + dispatcher**, the same pattern as the GPU backend
dispatcher (§9.4). v1 statically links the bundled backends and dispatches at launch.

**Requirements**
- **Registration.** Per-domain registration (`registerWindow` / `registerInput` /
  `registerAudio`); backends register at load. The null floor is always registered.
- **Selection.** For each domain, bind the **highest-`priority()` viable (`probe()`==
  true)** registered backend at launch.
- **Env override.** `CAJETA_IFX_WINDOW` / `CAJETA_IFX_INPUT` / `CAJETA_IFX_AUDIO` (and
  `=harness`) force a named backend, mirroring `CAJETA_XPU_BACKEND`.
- **The three cases (§9.4):**
  1. Real backend viable → bind it.
  2. Within-OS choice (Wayland vs X11, PipeWire vs Pulse vs ALSA) → `probe()`/`priority()`
     decide; env override wins.
  3. **Only the null floor present:** an app that *opts in* to headless gets `null`
     silently; an app that *requests an interactive surface* with only `null` available
     **fails loudly at launch** ("no `ifx.window` backend for this environment; add
     `ifx-backend`") — logged, never a silent black screen.

**Use cases**
- 4.1 — As an app on Linux with both Wayland and X11 backends linked, when `WAYLAND_DISPLAY`
  is set, then the Wayland backend binds; unset → X11 binds.
- 4.2 — As a developer, when I set `CAJETA_IFX_WINDOW=x11`, then X11 binds even if Wayland
  is viable.
- 4.3 — As an app that requests an interactive window with no OS backend linked, when it
  launches, then `ifx` raises a loud, logged launch error naming the missing backend.
- 4.4 — As a headless/CI job, when it opts into headless, then the null floor binds
  silently and gfx renders to an offscreen `Surface`.

---

## 5. Null / headless floor

Shipped in stdlib `ifx` so any program runs with no OS backend (§9.4 case 3).

**Requirements**
- `NullWindowBackend` / `NullInputBackend` / `NullAudioBackend`: always `probe()==true`,
  lowest `priority()`, `supports()==false` for all optional features.
- Null window: creates no OS window, returns an **offscreen `Surface`** (gfx renders to an
  image — real value for servers/CI/golden tests); yields no events.
- Null input: empty device set. Null audio: silent discard / silent capture.

**Use cases**
- 5.1 — As a server/CI job, when no backend is linked, then `ifx` still binds (the null
  floor) and the program runs headless.
- 5.2 — As gfx, when bound to the null window, then I render to the offscreen `Surface`
  and may download the framebuffer.

---

## 6. Capability, permission & lifecycle model

The contract is **floor + capability flags**; each backend fills what its OS offers and
reports the rest unsupported (§9.7).

**Requirements**
- **Capability query.** `Feature` enum + `ifx.supports(Feature)` per active backend;
  apps program to the floor and feature-detect (multi-window, window position, cursor
  warp→pointer-lock fallback, touch, rumble, adaptive triggers, loopback capture,
  exclusive audio, HDR present, …).
- **Permissions are first-class.** `Permission` + `PermissionState`: `ifx` exposes
  request + state for mic capture (mobile/macOS) and input-device access (Linux
  seat/udev-ACL). **Denied is a reported state, not a crash.**
- **Lifecycle + surface-loss are MANDATORY contract events.** `LifecyclePhase`:
  suspend/resume, surface-lost / surface-recreated (Android `APP_CMD_TERM_WINDOW`, iOS
  background force them; desktop emits minimize/restore). The gfx swapchain MUST handle
  surface-recreated — non-optional.
- **Audio route/interruption events are portable** so games pause/resume + rebuild the
  graph uniformly.

**Use cases**
- 6.1 — As an app, when I query `supports(CURSOR_WARP)` on Wayland, then it returns false
  and I use pointer-lock + relative motion instead.
- 6.2 — As an app, when I request mic capture and the user denies, then I observe a
  denied `PermissionState` (no exception, no capture).
- 6.3 — As an app on mobile, when the OS backgrounds me, then I receive a `LifecyclePhase`
  surface-lost event, and a surface-recreated event on return.

---

## 7. Recording SPI seam (harness & codecs)

`ifx` defines the **seam**; the implementations are external (§9.5–§9.6).

**Requirements**
- A `VideoSink` (and audio equivalent) SPI: "take frames / samples, write a recording."
- Stdlib ships only the **royalty-free fallback**: window → PNG/image sequence; audio →
  WAV/PCM. No codec in stdlib.
- The optional `cajeta-ifx-harness` backend (its own repo) implements all three domains
  as capture/replay and registers like any backend; the silent `null` floor stays in
  stdlib.
- Optional external codec libraries implement the seam and register; selection =
  registry + probe + priority + `CAJETA_IFX_VIDEO` override; graceful/loud per §9.4.

**Use cases**
- 7.1 — As a test, when I select the harness backend, then presented frames are recorded
  to a PNG sequence and the output mix to WAV, with no OS display/sound card.
- 7.2 — As an app with no codec library, when I request capture, then I get the PNG/WAV
  fallback (logged); demanding a codec-only format with no encoder present is a loud error.

---

## 8. Interop boundary (informative)

`ifx`'s contract stays **pure C-shaped**; the per-platform binding strategy (direct C FFI
on Windows/Linux; an Obj-C shim on macOS/iOS; a JNI companion on Android — §9.8) lives
**in each backend**, never in `ifx`. No backend requires Cajeta to understand Obj-C or
Java — all reduce to the C ABI that `@Native` already provides. Per-backend interop
detail lives in each `cajeta-ifx-*` repo's spec/plan (Appendix B — Interop).

---

## 9. References

- Umbrella & rationale: `gfx-spec.md` §9.1–§9.8.
- Contract impl: `runtime/src/cajeta/ifx/` (stdlib).
- Backends: `cajeta-ifx-{windows,linux,macos,ios,android}`, `cajeta-ifx-harness`.
- Melt (build-target selector): `cajeta-ifx-backend`.
- Min-OS floors & feature matrix: §9.7.
