# `cajeta.io.file.Watcher` — filesystem notifications

Streaming filesystem change events. Backed by `inotify` (Linux),
`FSEvents` (mac), and `ReadDirectoryChangesW` (Windows) under one
interface. Fiber-parks on read — calling code blocks the
underlying fiber, not an OS thread.

Phase: **deferred from the v1 implementation rollout.** The classes
exist as placeholders so the package structure mirrors the spec,
but the notifier itself is not built — there is no inotify /
FSEvents / ReadDirectoryChangesW backing, no async-runtime
fiber-park, and no `Stream<FileEvent>` wiring yet. Signatures will
shift when the backing lands.

## Surface (current placeholders)

`WatchKind` and `FileEvent` carry the event vocabulary; `Watcher`
today only opens, holds a backing `handle`, and closes. The
attributes are **public fields** (no accessor methods), and
`FileEvent.kind` carries the `WatchKind` ordinal as an `int32`.

```cajeta
public enum WatchKind { CREATE, MODIFY, DELETE, RENAME }

public class FileEvent {
    public Path  path;
    public int32 kind;                  // WatchKind ordinal
    public int64 timestampNanos;        // Unix nanoseconds
    public Path  renameTarget;          // null when kind != RENAME

    public FileEvent(Path path, int32 kind, int64 timestampNanos, Path renameTarget);
}

public class Watcher {
    public int64 handle;                // inotify fd / event-stream ptr

    public Watcher();                   // no active backing handle
    public void close();                // releases the handle
}
```

## Surface (planned)

The streaming notifier API — not yet present:

```cajeta
public class Watcher {
    public static Watcher of(Path root);
    public Stream<FileEvent> events();   // fiber-parks on read
}
```

`Instant`-typed `timestamp` and `WatchKind`-typed `kind` accessors
are planned once `Instant` lands (the field stays `int64` Unix
nanoseconds until then).

## Notes (planned behavior)

- **One Watcher per root.** `Watcher.of(p)` would watch `p`
  recursively. To watch multiple roots, create one Watcher per root
  and merge their event streams.
- **Recursion** — recursive by default (matches FSEvents). inotify
  doesn't natively recurse — the implementation would walk the tree
  at construction and add watches per directory, attaching watches
  to newly-created subdirectories on the fly.
- **Rename detection** — kernel-supported on Linux/Windows; on
  macOS, `FSEvents` reports both halves and the Watcher would
  correlate them in a small ring buffer.
- **Backpressure** — the events stream would block the fiber when
  full; lost events would surface as a synthetic overflow kind
  (the enum would gain the variant) so clients drop their cached
  view and rescan.

## Idiomatic use (planned)

```cajeta
Watcher w = Watcher.of(Path.of("src"));
for (FileEvent e : w.events().take(100)) {
    if (e.kind == WatchKind.MODIFY) { reindex(e.path); }
}
w.close();
```

Dispatch is by comparing the `int32` `kind` against the named
`WatchKind` constants (their ordinals are stable).

## See also

- [`Path.md`](Path.md) — the watched root.
- [`Directories.md`](Directories.md) — one-shot traversal
  alternative.
