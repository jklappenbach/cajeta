# `cajeta.io.file.FileInfo` — batched stat result

`FileInfo` is the value-typed snapshot of a file's metadata at the
moment `Path.info()` was called. Use it when multiple stat
questions matter — one syscall, every answer cached. For one-shot
predicates use `Path.exists()` / `isFile()` / etc. directly.

## Surface

```cajeta
public final value class FileInfo {
    public int64   size();              // bytes
    public Instant created();
    public Instant modified();
    public Instant accessed();
    public boolean isFile();
    public boolean isDir();
    public boolean isSymlink();
    public int32   permissions();       // POSIX mode bits
}
```

## Notes

- **Value class** — `FileInfo` is laid out inline in the
  caller's stack frame; no heap alloc, no drop. Copies are
  cheap.
- **`Instant`** — a single point on the monotonic-or-wall clock
  timeline. See `cajeta-docs/stdlib/Time.md`. v1 may model
  `created()` / `modified()` / `accessed()` as `int64` Unix
  nanoseconds before the full `Instant` class lands; signatures
  flip to `Instant` once it exists.
- **`permissions()`** — POSIX mode bits (`0o755`, etc.) on
  Linux/mac. Windows returns a synthesized triple computed from
  the security descriptor — same bit layout, lossy.

## See also

- [`Path.md`](Path.md) — the source of `FileInfo`.
- `cajeta-docs/stdlib/Time.md` — `Instant`.
