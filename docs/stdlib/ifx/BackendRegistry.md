# BackendRegistry

`cajeta.ifx.BackendRegistry` — the ifx keystone: a registry + probe +
dispatcher, the same pattern as the GPU backend dispatcher. Backends register
themselves at load through the per-domain entry points; at launch the registry
binds, per domain independently, the highest-`priority()` backend whose
`probe()` reports it viable in the current environment (ties: first registered
wins, deterministically). The window, input, and audio domains are registered
and selected separately, and an always-present null floor
(`NullWindowBackend` / `NullInputBackend` / `NullAudioBackend`) means
selection never comes up empty. When only the floor is viable for the window
domain, an opt-in headless request binds it silently; an interactive request
fails loudly with `IfxException` rather than hand back a silent black screen.
A `CAJETA_IFX_WINDOW` / `_INPUT` / `_AUDIO` env variable forces a registered
backend by name; an unknown name is a loud launch error.

```cajeta
BackendRegistry reg = BackendRegistry.instance();
WindowBackend win = reg.selectWindow(true);            // headless: floor allowed
boolean rumble = reg.supportsInput(Feature.GamepadRumble);
```

## Methods

| Signature | |
|---|---|
| `static BackendRegistry instance()` ⚑ | The shared process-wide registry, with the null floor auto-registered the first time it is touched |
| `BackendRegistry()` | Construct an empty registry (the shared one comes from `instance()`) |
| `void registerWindow(WindowBackend backend)` | Register a window-domain backend (called by each backend at load) |
| `void registerInput(InputBackend backend)` | Register an input-domain backend |
| `void registerAudio(AudioBackend backend)` | Register an audio-domain backend |
| `WindowBackend selectWindow(boolean headless)` ⚑ | Bind the window backend: highest-`priority()` viable; `headless` opts into the null floor, an interactive request fails loudly on floor-only |
| `InputBackend selectInput()` | Bind the input backend: highest-`priority()` viable, the null floor (empty devices) at worst |
| `AudioBackend selectAudio()` | Bind the audio backend: highest-`priority()` viable, the silent null floor at worst |
| `boolean supportsWindow(Feature feature)` | Does the backend the window domain would bind provide `feature`? |
| `boolean supportsInput(Feature feature)` | Does the backend the input domain would bind provide `feature`? |
| `boolean supportsAudio(Feature feature)` | Does the backend the audio domain would bind provide `feature`? |

⚑ = `@EntryPoint`

## See also

- [Window](Window.md) — the portable window contract backends issue handles for
- Source: [`runtime/src/cajeta/ifx/BackendRegistry.cajeta`](../../../runtime/src/cajeta/ifx/BackendRegistry.cajeta)
