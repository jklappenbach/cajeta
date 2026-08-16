---
id: io-file-streaming
applies-to: [cajeta/io/file/FileReader, cajeta/io/file/FileWriter]
title: FileReader / FileWriter — streaming file I/O (open → loop → close)
description: Streaming byte read/write via File.openRead/openWrite; EOF is a short/0 return (not an exception), no user-space buffer, you MUST call close() — no auto-close-on-drop yet.
---

# Streaming file I/O — `FileReader` + `FileWriter`

The streaming pair for pulling/pushing bytes through a live OS fd without holding
the whole file in memory. Use this when the file is large or unbounded and you
want a fixed-size buffer; for whole-file-in-one-call use `File.readAllBytes` /
`File.writeAllBytes`, and for seekable positional access use the `File` instance
handle (`File.open`). All three live in `cajeta.io.file`.

You do **not** construct these — obtain them from the `File` factories:
`File.openRead(path)` → `#FileReader` (read-only), `File.openWrite(path, mode)` →
`#FileWriter` (mode is an `OpenMode`). The internal `FileReader(int32 fd)` /
`FileWriter(int32 fd)` ctors are wired by those factories only.

## Members and roles

- **`FileReader`** — read side. `read(int8[] buf, int32 max) -> int32` fills
  `buf[0..max)` and returns the count; `readString(int32 maxBytes) -> #String`
  pulls bytes as UTF-8; `position() -> int64` is cumulative bytes consumed;
  `close()`. Public fields `fd` (`-1` once closed) and `pos`.
- **`FileWriter`** — write side. `write(int8[] data, int32 len) -> void`,
  `writeString(String s) -> void`, `flush() -> void`, `close()`. Same `fd`/`pos`
  fields.

The two never reference each other — a reader and a writer are independent handles
over (possibly) the same path. They are joined only through the `File` factory
that mints them and through `OpenMode` (writer side).

## Ownership across the boundary

- The factory returns an **owned** handle (`#FileReader` / `#FileWriter`): the
  caller owns the fd. Move it with `#` if you hand it off.
- `read(buf, max)` / `write(data, len)` **borrow** the caller's array — there is
  no user-space buffer; bytes go straight to/from the syscall into your array.
  Ownership of `buf`/`data` stays with the caller; you allocate and free it.
- `readString(maxBytes)` returns an **owned `#String`** backed by a freshly
  allocated buffer, so it survives `close()` of the reader.
- **You MUST call `close()`.** There is no auto-close-on-drop destructor yet, so
  letting the handle go out of scope leaks the fd. `close()` is idempotent and
  sets `fd = -1`. (`FileWriter.close()` flushes first.)

## The read loop — EOF protocol

EOF is reported as a **short or zero return from `read`, never an exception.** The
native read drains OS-level short reads, so a return `< max` (including 0) means
EOF was reached on that call; after EOF the file stays open and further reads
return 0. Loop until the return is 0:

```cajeta
import cajeta.io.file.File;
import cajeta.io.file.FileReader;

FileReader r #= File.openRead("/var/log/app.log");
int8[] buf = heap int8[4096];
int32 n = r.read(buf, 4096);
while (n != 0) {
    // consume buf[0..n); r.position() has advanced by n
    n = r.read(buf, 4096);
}
r.close();
```

Do **not** expect `EndOfFileException` here — that subtype is thrown only by
higher-level all-or-error helpers, not by `FileReader.read`. A negative return
would mean a hard IO error, but the body throws an `IoException` on error rather
than returning a sentinel, so it should never surface to the loop.

## The write sequence

```cajeta
import cajeta.io.file.File;
import cajeta.io.file.FileWriter;
import cajeta.io.file.OpenMode;

FileWriter w #= File.openWrite("/tmp/out.txt", OpenMode.WRITE);   // truncate-or-create
w.writeString("first line\n");
w.writeString("second line\n");
w.flush();
w.close();
```

`write`/`writeString` loop past partial OS writes, so when they return every
requested byte is in the kernel. Pick the `OpenMode` at open time: `WRITE`
(truncate), `APPEND`, `CREATE_NEW` (exclusive), etc. — see the `OpenMode` enum.

## What this component does NOT do (don't hunt for these)

- **No seek / no random access.** `FileReader.position()` is read-only progress;
  there is no rewind. Need positional I/O? Use the `File` instance handle.
- **No durability flush.** `FileWriter.flush()` is a no-op (writes are already
  unbuffered) and does **not** `fsync`. For crash-durable flushing use
  `File.sync()` on a `File` handle.
- **No user-space buffering.** Every `read`/`write` is a syscall path; batch with
  a reasonably sized array yourself.
- **No charset decode.** `readString` takes bytes as UTF-8 verbatim; transcode
  non-UTF-8 content via `Encoding` yourself.
- **No auto-close.** Reiterated because it is the costliest miss — close it.
