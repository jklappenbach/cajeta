# Cajeta ifx tour

The `cajeta.ifx` platform-backend contract (window / input / audio), driven
headlessly against the **Null backend floor** — so it runs anywhere: no display
server, no GPU, no audio device.

This is a companion entry point to the language tour one folder up
(`samples/tour/`). It lives in its own subfolder because backend selection is
an environment-dependent concern; here the environment dependency is removed
by design — the Null floor (`name() == "null"`, priority `-1000`, `probe()`
always true) is auto-registered by `BackendRegistry.instance()`, so every
demo is deterministic and self-checks.

```
samples/tour/ifx/
├── README.md
├── cajeta.json                  ← build-tool manifest (build / run / clean)
├── run-ifx.sh                   ← cajeta build + execute
└── src/tour/ifx/IfxTour.cajeta  ← the demo (package tour.ifx)
```

## Run it

The compiler must be built first (`cd <repo> && ./build.sh`). Then:

```sh
./run-ifx.sh
```

The exit code is the tour's self-check result (0 = all passed).

## What it demonstrates

- **Selection** — `BackendRegistry.instance().selectWindow(true)` binds the
  highest-priority viable backend; with only the floor registered that is
  `"null"` (priority −1000, `probe()` true, `supports()` nothing).
- **The loud-fail policy** — `selectWindow(false)` (an interactive session) on
  a floor-only registry throws `IfxException` rather than handing the app a
  silent black screen.
- **The window contract against Null** — `createWindow` / `surfaceOf` /
  `poll` / `pollLifecycle` return `null`, `destroy` is a no-op; plus a
  headless `Surface` value (`nativeHandle == 0` ⇒ `isHeadless()`).
- **`IfxInfo`** — the headless-tolerant facade snapshot: contract `version()`
  and the bound backend name per domain (all `"null"` on a floor-only host).

`CAJETA_IFX_WINDOW` / `_INPUT` / `_AUDIO` env vars force a named backend at
selection time (which is why the manifest declares the `env` capability).

## Expected output

```
=== Cajeta ifx tour ===
  the window/input/audio backend contract, headless
  against the always-viable Null floor.

-- backend selection: headless bind to the Null floor --
  selectWindow(headless=true) -> name='null' priority=-1000 probe=true

-- policy: interactive request on a floor-only registry throws --
  selectWindow(headless=false) threw IfxException (as specified)

-- window contract: Null backend no-ops --
  createWindow/poll/pollLifecycle/surfaceOf -> null; destroy() no-op
  Surface(handle=0).isHeadless() = true, 640x480

-- IfxInfo: facade snapshot --
  version=1 window='null' input='null' audio='null'

=== ifx tour complete: 11 self-checks passed ===
```

See `runtime/src/cajeta/ifx/skills/ifx-overview.md` for the contract design
and `docs/stdlib/ifx/` for the API reference.
