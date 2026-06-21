---
id: io-file-File-writeAllBytes
applies-to: [cajeta/io/file/File.writeAllBytes]
title: File.writeAllBytes — atomic one-shot whole-file write
description: Write a byte buffer to a path in one call with atomic-rename semantics; does not create parent dirs.
---

# `File.writeAllBytes` — whole-file write in one call

```cajeta
public static void writeAllBytes(String path, int8[] data, int32 len)
```

Writes the first `len` bytes of `data` to `path` and returns nothing. The target
is created if absent and **truncated** if it already exists; the write lands via
atomic rename, so a reader sees either the old file or the complete new one,
never a half-written file. Use it when the whole payload fits in memory; for
incremental output use `File.openWrite` (→ `cajeta/io/file/FileWriter`).

## Example (with imports)

```cajeta
import cajeta.io.file.File;

int8[] data = { (int8) 72, (int8) 101, (int8) 108, (int8) 108, (int8) 111 }; // "Hello"
File.writeAllBytes("/tmp/out.txt", data, 5);
```

Round-trips with the owned read side:

```cajeta
#int8[] back = File.readAllBytes("/tmp/out.txt"); // caller owns `back`
```

## Parameters & ownership

- `path` — destination. **Borrowed** (no `#`): not retained or freed by the call.
- `data` — source bytes. **Borrowed**: the caller keeps ownership and frees it as
  usual at scope exit; this method neither stores nor frees it.
- `len` — number of bytes to write, counted from index 0. Pass the actual count
  you want written, **not** the array's capacity — `data` may be larger than
  `len`, and only `data[0 .. len)` is written. `len` must be `<= data.count()`.

This method transfers nothing and returns nothing, so there is no ownership to
track on the result. Contrast `readAllBytes`, which returns a `#`-owned `int8[]`
the caller must free.

## What it does NOT do

- **Does not create parent directories.** Writing to `/tmp/new/out.txt` when
  `/tmp/new` is missing fails — there is no `mkdir` here. Ensure the directory
  exists first.
- Does not append — an existing file is truncated, not extended. For append use
  `File.openWrite(path, OpenMode.APPEND)`.
- Does not buffer or return a handle; it is fully synchronous and durable on
  return (the atomic rename is the commit point).

## Failure modes

Raises the `cajeta/io/file/IoException` family (`RecoverableException` subtypes —
catch the specific type or declare it in your throws clause). The errno-mapped
subtypes that apply to a write:

- `NotFoundException` — a path component (e.g. a missing parent dir) doesn't exist.
- `PermissionException` — no write permission on the directory/file.
- `IsDirectoryException` — `path` names an existing directory.
- `DiskFullException` — out of space mid-write.

```cajeta
import cajeta.io.file.File;
import cajeta.io.file.IoException;

try {
    File.writeAllBytes(path, data, len);
} catch (IoException e) {
    log.warn(e.message);
}
```

See `cajeta/io/file/IoException` for the full errno→exception chart.
