# `cajeta.io.file.Watcher` — filesystem notifications

Streaming filesystem change events. Backed by `inotify` (Linux),
`FSEvents` (mac), and `ReadDirectoryChangesW` (Windows) under one
interface. Fiber-parks on read — calling code blocks the
underlying fiber, not an OS thread.

Phase: **deferred from the v1 implementation rollout.** Spec
captured here; signatures may shift modestly when the async
runtime backing lands.

## Surface

```cajeta
public enum WatchKind { CREATE, MODIFY, DELETE, RENAME }

public final value class FileEvent {
    public Path      path();
    public WatchKind kind();
    public Instant   timestamp();
    public Path      renameTarget();             // null when kind != RENAME
}

public final class Watcher {
    public static Watcher of(Path root);

    public Stream<FileEvent> events();           // fiber-parks on read
    public void close();
    public ~Watcher();
}
```

## Notes

- **One Watcher per root.** `Watcher.of(p)` watches `p`
  recursively. To watch multiple roots, create one Watcher per
  root and merge their event streams.
- **Recursion** — recursive by default; this matches FSEvents
  semantics. inotify doesn't natively recurse — the
  implementation walks the tree at construction time and adds
  watches per directory. Newly-created subdirectories get
  watches added on the fly.
- **Rename detection** — kernel-supported on Linux/Windows; on
  macOS, `FSEvents` reports both halves and the Watcher
  correlates them in a small ring buffer.
- **Backpressure** — the events stream blocks the fiber when
  full. Lost events surface as a synthetic
  `FileEvent.kind == OVERFLOW` (TODO once the enum gains the
  variant) — clients drop their cached view and rescan.

## Idiomatic use

```cajeta
Watcher w = Watcher.of(Path.of("src"));
for (FileEvent e : w.events().take(100)) {
    println(e.path() + " " + e.kind());
}
```

## See also

- [`Path.md`](Path.md) — the watched root.
- [`Directories.md`](Directories.md) — one-shot traversal
  alternative.
