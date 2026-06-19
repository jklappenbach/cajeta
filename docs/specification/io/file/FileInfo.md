# `cajeta.io.file.FileInfo` — batched stat result

`FileInfo` is a snapshot of a file's metadata captured in one
`stat` call: size, timestamps, type flags, and permission bits for
one filesystem entry. Use it when multiple stat questions matter —
one syscall, every answer cached. For one-shot predicates use
`Path.exists()` / `isFile()` / etc. directly.

## Surface

The attributes are plain **public fields**, read directly off an
instance (no accessor methods):

```cajeta
public class FileInfo {
    public int64   size;             // bytes
    public int64   createdNanos;     // Unix nanoseconds
    public int64   modifiedNanos;    // Unix nanoseconds
    public int64   accessedNanos;    // Unix nanoseconds
    public boolean isFile;
    public boolean isDir;
    public boolean isSymlink;
    public int32   permissions;      // POSIX mode bits

    public FileInfo();               // zero-initialized
}
```

```cajeta
FileInfo meta = heap FileInfo();
if (meta.isFile) {
    int64 bytes = meta.size;
    int64 mtime = meta.modifiedNanos;   // Unix nanoseconds
}
```

## Notes

- **Phase C placeholder** — the only constructor today is the
  zero-initialized no-arg form. A stat-touching accessor on `Path`
  that returns a populated `FileInfo` in one syscall is *planned*
  (the native `__cajeta_path_stat` helper exists but no `Path`
  method is wired to it yet). Until then, construct one directly
  and fill it in.
- **Timestamps** — exposed as `int64` Unix nanoseconds
  (`createdNanos` / `modifiedNanos` / `accessedNanos`). `Instant`-
  typed accessors (`created` / `modified` / `accessed`) are
  planned for when the `Instant` class lands
  (see `docs/specification/time/Time.md`); the field swap won't break callers
  reading the nanos fields.
- **`permissions`** — POSIX mode bits (`0o755`, etc.) on Linux/mac.
- **Plain class** — heap-allocated like any class (`heap
  FileInfo()`); there is no `value class` form in the language.

## See also

- [`Path.md`](Path.md) — the planned source of populated
  `FileInfo`; the cheaper single-attribute probes (`isFile()` /
  `isDir()` / `isSymlink()`).
- `docs/specification/time/Time.md` — `Instant`.
