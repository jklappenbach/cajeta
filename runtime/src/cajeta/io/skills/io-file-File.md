---
id: io-file-File
applies-to: [cajeta/io/file/File]
title: File — filesystem access point (one-shots, streaming factories, seekable handle)
description: Pick whole-file readAllBytes/writeAllBytes, streaming openRead/openWrite, or a seekable open(path,OpenMode) handle you must close().
---

# File

The single entry point for filesystem access in `cajeta.io.file`. **Access point: yes** —
you never construct `File` yourself; you call its static factories. It exposes three
patterns under one class name — pick by what you need:

| Task | Use | Returns |
| --- | --- | --- |
| Read/write a whole file in one call | `File.readAllBytes` / `File.writeAllBytes` | `#int8[]` / `void` |
| Stream a file (sequential read/write loop) | `File.openRead` / `File.openWrite` | `#FileReader` / `#FileWriter` |
| Random access (seek, lock, truncate, sync, positional read/write) | `File.open(path, OpenMode)` | `#File` instance handle |
| Atomic create-or-fail (lock file) | `File.openExclusive(path)` | `#File` instance handle |

`OpenMode` (`READ`, `WRITE`, `APPEND`, `READ_WRITE`, `CREATE_NEW`) selects intent for the
writer and seekable factories; `openRead` is always read-only and takes no `OpenMode`.

## Construction & ownership

Do **not** `heap File(...)` — the only public ctor is internal plumbing wired by
`File.open`. Every factory **returns a `#`-owned value** that the caller owns:

- `readAllBytes` returns a `#int8[]` sized exactly to the file's contents.
- `openRead`/`openWrite` return `#FileReader`/`#FileWriter` (see those classes for the
  streaming API); `EndOfFileException` and the rest of the hierarchy come from them.
- `open`/`openExclusive` return a `#File` handle that **owns its OS fd** — you must
  `close()` it (or let its destructor run). The handle is the only one of these returns
  with a lifecycle obligation.

## Worked example — seekable handle (mirrors FileIoTests)

```cajeta
import cajeta.io.file.File;
import cajeta.io.file.OpenMode;

#File f = File.open("/tmp/db.bin", OpenMode.READ_WRITE);
f.seek(4096);                       // absolute byte offset
int8[] page = heap int8[512];
int64 n = f.read(page, 0, 512);     // -> bytes filled (0 == EOF)
int8[] data = heap int8[3];
f.write(data, 0, 3);                // writes at current position
f.sync();                           // fsync(2): durable before return
f.close();                          // idempotent; destructor also closes
```

One-shot variant:

```cajeta
import cajeta.io.file.File;

#int8[] all = File.readAllBytes("/etc/hostname");
int8[] out = heap int8[5];
File.writeAllBytes("/tmp/out.bin", out, 5);   // pass length explicitly; atomic rename
```

## Instance handle — the methods that matter

The handle is a mutable seekable cursor (`fd` + cached `pos`); not reusable after
`close()` and not thread-safe — one fiber/thread per handle.

- `int64 read(int8[] dst, int64 offset, int64 length)` — fills `dst[offset..offset+length)`
  from the current position; returns count filled (`0` == EOF, `-1` == hard error). Note
  this is the **three-arg** instance form; `FileReader.read` is a different two-arg form.
- `int64 write(int8[] data, int64 offset, int64 length)` — writes at current position;
  returns count written (`length` on success, `-1` on error).
- `seek(int64 absolute)` / `seekFromEnd(int64 offset)` (`0` lands at EOF) — both update `pos`.
- `int64 position()` — reads cached `pos`, no syscall; `int64 size()` — always one `fstat`.
- `truncate(int64 size)` — extends (sparse) or shrinks; does **not** move `pos`.
- `sync()` — `fsync(2)`, the crash-recovery primitive; `flush()` is a no-op (no user-space buffer).
- `lock()` / `tryLock()` / `unlock()` — advisory POSIX `flock LOCK_EX`, intra-process only,
  released on `close()`.

## Errors

Failures surface as `IoException` subtypes in `cajeta.io.file`: `NotFoundException`
(ENOENT), `PermissionException`, `AlreadyExistsException` (e.g. `CREATE_NEW` /
`openExclusive` on an existing path), `IsDirectoryException`, `NotDirectoryException`,
`DiskFullException`, `CrossDeviceException`. Catch the specific subtype or declare it in
your throws clause. (In the current build `read`/`write` also signal hard errors with a
`-1` sentinel; treat that as transitional.)

## What File does NOT do (don't hunt for these)

- **No directory operations** — no `mkdir`, listing, rename, delete, or stat-of-a-dir
  here; `writeAllBytes`/`open` will **not** create parent directories.
- **No path manipulation** — joining/normalizing lives in `cajeta/io/file/Path`.
- **No filesystem watching** — that is `cajeta/io/file/Watcher` + `WatchKind`/`FileEvent`.
- **No user-space buffering** — `flush()` is a no-op; use `sync()` for durability.
