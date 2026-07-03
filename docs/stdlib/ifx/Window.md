# Window

`cajeta.ifx.Window` — the portable window contract: one API across every OS,
with the concrete behaviour supplied by a backend (cajeta-ifx-*) selected by
the [backend registry](BackendRegistry.md). On platforms without desktop
windows (iOS/Android) a "window" is the single fullscreen surface. This slice
defines the type and its intrinsic accessors; the registry-delegating factory
(`create`) and event `poll` land with the `WindowBackend` dispatch.

```cajeta
Window w = heap Window((int64) 0);
int64 h = w.handle();
```

## Methods

| Signature | |
|---|---|
| `Window(int64 backendHandle)` ⚑ | Wrap a backend-issued window handle (backends construct these; apps get one from the registry-bound `WindowBackend`) |
| `int64 handle()` | The opaque per-backend handle (the backend interprets it) |

⚑ = `@EntryPoint`

## See also

- [BackendRegistry](BackendRegistry.md) — selects the backend that issues window handles
- Source: [`runtime/src/cajeta/ifx/Window.cajeta`](../../../runtime/src/cajeta/ifx/Window.cajeta)
