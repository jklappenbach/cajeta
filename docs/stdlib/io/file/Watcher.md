# Watcher

`cajeta.io.file.Watcher` — filesystem-change notifier: watches a
[Path](Path.md) or [File](File.md) and surfaces `FileEvent`s (created /
modified / deleted, filtered by `WatchKind`) as they happen. The full watcher
is a deferred phase — today a `Watcher` only opens, holds a backing `handle`
(inotify fd on Linux, event-stream pointer elsewhere), and closes; the native
backing, fiber-park integration, and `Stream<FileEvent>` wiring are still to
come.

```cajeta
Watcher watcher = heap Watcher();
// ... future: drain a Stream<FileEvent> here ...
watcher.close();
```

## Methods

| Signature | |
|---|---|
| `Watcher()` ⚑ | Creates a `Watcher` with no active backing handle |
| `void close()` | Releases the backing handle; no-op while the watcher is inactive |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/file/Watcher.cajeta`](../../../../runtime/src/cajeta/io/file/Watcher.cajeta)
- [Path](Path.md) / [File](File.md) — what a watcher observes
