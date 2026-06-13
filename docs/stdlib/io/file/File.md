# `cajeta.io.file.File` — one-shot statics + random-access handle

`File` covers two patterns under one class name:

1. **Static one-shots** — `readAllBytes` / `writeAllBytes` / the
   `openRead` / `openWrite` factories that return streaming
   classes. Cover ~80% of usage.
2. **Instance methods** (Phase E) — random-access I/O on a held
   file handle: seek, position, lock, sync. Returned by the
   `open` / `openExclusive` factories.

The two surfaces share one class because callers conceptually
"want a file" — splitting into `Files` (statics) + `File`
(instance) is Java's mistake.

## Surface — one-shot statics + streaming opens

```cajeta
public class File {
    // One-shot shortcuts.
    public static #int8[]     readAllBytes(String path);
    public static void        writeAllBytes(String path, int8[] data, int32 len);

    // Streaming opens. Caller drives the byte loop.
    public static #FileReader openRead(String path);
    public static #FileWriter openWrite(String path, OpenMode mode);
}
```

Every factory takes a `String path`. A `Path`-typed overload is
planned but not yet wired.

## Surface — random-access handle (Phase E)

```cajeta
public class File {
    public int32 fd;                                  // OS fd, -1 once closed
    public int64 pos;                                 // cached position

    public static #File open(String path, OpenMode mode);
    public static #File openExclusive(String path);   // O_CREAT | O_EXCL

    public int64 read(int8[] dst, int64 offset, int64 length);
    public int64 write(int8[] data, int64 offset, int64 length);

    public int64 position();
    public void  seek(int64 absolute);
    public void  seekFromEnd(int64 offset);
    public int64 size();
    public void  truncate(int64 size);
    public void  flush();                             // no-op (no user buffer)
    public void  sync();                              // fsync
    public void  lock();
    public boolean tryLock();
    public void  unlock();

    public void  close();                             // also called by drop
}
```

`read` / `write` return the byte count transferred (`int64`); a `0`
return from `read` is EOF. A `Buffer`-taking overload and the
`InputStream` / `OutputStream` interfaces are planned, not yet on
the class.

## Notes

- **`#` return ownership** — `readAllBytes` returns a heap-
  allocated `int8[]` (the caller takes ownership), so the
  multi-param + reference-return shape doesn't trip the
  borrow-return-multi-param check. Same for `openRead` /
  `openWrite` — the returned `FileReader` / `FileWriter` is
  owned.
- **Atomic write** — `writeAllBytes(path, data, len)` writes to
  `<path>.tmp.<pid>`, fsyncs, then renames over `path` (atomic on
  POSIX). Concurrent readers either see the OLD file or the NEW
  file, never a half-written one. On any failure the tmp file is
  unlinked and the destination is left untouched. A direct-write
  overload (skip the temp+rename dance) is planned, not yet
  present.
- **Error reporting today** — the native helpers return
  sentinels, not exceptions: `readAllBytes` yields `null` on a
  missing / unreadable path, `writeAllBytes` and the instance
  `read` / `write` return `-1` on hard error. The
  [`IoException`](Errors.md) hierarchy is defined but not yet
  thrown from these call sites; throwing wrappers land once the
  hierarchy is wired end-to-end.
- **Drop auto-closes** — the random-access `File` instance and
  the streaming `FileReader` / `FileWriter` all close on scope
  exit via the destructor chain (idempotent: `close()` sets
  `fd = -1`, so the destructor skips an already-closed fd).
  Explicit `close()` is only for early release.
- **`InputStream` / `OutputStream` interfaces** — planned for the
  random-access `File` so generic streaming code can work against
  it; the interfaces are not declared on the class yet.

## See also

- [`FileReader.md`](FileReader.md),
  [`FileWriter.md`](FileWriter.md) — what `openRead` /
  `openWrite` return.
- [`OpenMode.md`](OpenMode.md) — the enum passed to
  `openWrite` / `open`.
- [`Path.md`](Path.md) — the path value type (planned arg overloads).
- [`Errors.md`](Errors.md) — `NotFoundException`,
  `PermissionException`, `IsDirectoryException`, etc.
