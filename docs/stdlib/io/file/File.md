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
    public static #int8[] readAllBytes(Path p);
    public static #int8[] readAllBytes(String path);
    public static void    writeAllBytes(Path p, int8[] data, int32 len);
    public static void    writeAllBytes(String path, int8[] data, int32 len);

    // Streaming opens. Caller drives the byte loop.
    public static #FileReader openRead(Path p);
    public static #FileReader openRead(String path);
    public static #FileWriter openWrite(Path p, OpenMode mode);
    public static #FileWriter openWrite(String path, OpenMode mode);
}
```

## Surface — random-access handle (Phase E)

```cajeta
public final class File implements InputStream, OutputStream {
    public static File open(Path p, OpenMode mode);
    public static File openExclusive(Path p);        // O_CREAT | O_EXCL

    public int64 read(Buffer dst);
    public int64 write(Buffer src);
    public int64 read(int8[] dst, int64 offset, int64 length);
    public int64 write(int8[] src, int64 offset, int64 length);

    public int64 position();
    public void  seek(int64 absolute);
    public void  seekFromEnd(int64 offset);
    public int64 size();
    public void  truncate(int64 size);
    public void  flush();
    public void  sync();                              // fsync
    public void  lock();
    public boolean tryLock();
    public void  unlock();

    public void  close();                             // also called by drop
}
```

## Notes

- **`#` return ownership** — `readAllBytes` returns a heap-
  allocated `int8[]` (the caller takes ownership), so the
  multi-param + reference-return shape doesn't trip the
  borrow-return-multi-param check. Same for `openRead` /
  `openWrite` — the returned `FileReader` / `FileWriter` is
  owned.
- **Atomic write** — `writeAllBytes(p, data, len)` writes to
  `<p>.tmp.<rand>`, fsyncs, then renames over `p` (atomic on
  POSIX + NTFS). Concurrent readers either see the OLD file
  or the NEW file, never a half-written one. Direct write
  (skip the temp+rename dance) is available later via a
  `writeAllBytesDirect` overload.
- **String overloads** — every `Path`-taking static has a
  `String path` overload that internally calls `Path.of(path)`.
  Saves the `Path.of` ceremony in one-shot reads.
- **Drop auto-closes** — the random-access `File` instance and
  the streaming `FileReader` / `FileWriter` all close on scope
  exit via the destructor chain. Explicit `close()` is only for
  early release.
- **`InputStream` / `OutputStream` interfaces** — the random-
  access `File` implements both (Phase E), so generic streaming
  code works against it identically.

## See also

- [`FileReader.md`](FileReader.md),
  [`FileWriter.md`](FileWriter.md) — what `openRead` /
  `openWrite` return.
- [`OpenMode.md`](OpenMode.md) — the enum passed to
  `openWrite` / `open`.
- [`Path.md`](Path.md) — the `Path` arg type.
- [`Errors.md`](Errors.md) — `NotFoundException`,
  `PermissionException`, `IsDirectoryException`, etc.
