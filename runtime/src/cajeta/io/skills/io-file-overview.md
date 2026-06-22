---
id: io-file-overview
applies-to: [cajeta/io/file]
title: cajeta.io.file — filesystem access, paths, streaming I/O, and the IoException family
description: Package map for cajeta.io.file — pick File one-shot vs streaming vs random-access, pure-path queries on Path, and the IoException error hierarchy.
---

# cajeta.io.file

Filesystem package. Read/write bytes through `File`, manipulate path strings
through `Path`, stream through `FileReader`/`FileWriter`, select intent with
`OpenMode`, and catch failures from the `IoException` hierarchy. Pure-path
string work needs no syscall and no error handling; everything that touches the
disk goes through `File`.

## Pick your access point (routing)

| You want to… | Use | Notes |
|---|---|---|
| Read a whole file in one call | `File.readAllBytes(path) -> #int8[]` | allocates the result; you own it |
| Write a whole buffer in one call | `File.writeAllBytes(path, data, len)` | atomic-rename semantics; does NOT create parent dirs |
| Stream a file in chunks | `File.openRead(path) -> #FileReader` | loop `read(buf, max)` until it returns 0 (EOF) |
| Append/overwrite incrementally | `File.openWrite(path, mode) -> #FileWriter` | `mode` is an `OpenMode` |
| Seek/lock/truncate/sync (random access) | `File.open(path, mode) -> #File` | the instance handle; has `seek`, `lock`, `sync`, `truncate` |
| Create-or-fail (lock file) | `File.openExclusive(path) -> #File` | `O_CREAT|O_EXCL` |
| Query a path string (name/stem/ext/parent/join) | `Path.of(s)` then pure methods | no syscall, no throw |
| mkdir -p / delete / symlink / chmod+x / realpath | `Path` mutators (`mkdirs`, `delete`, `symlinkTo`, `setExecutable`, `canonical`) | Phase C/D, intrinsic-lowered |

Negative rows — capabilities that are **not here**:
- **No directory listing / glob / walk.** There is no `readdir`/`Stream<Path>`
  yet; `Path.delete()` will not recurse non-empty dirs.
- **No filesystem watching in v1.** `Watcher`, `FileEvent`, `WatchKind` are
  **deferred placeholders** — the inotify/FSEvents backing is unbuilt. Do not
  route real change-notification work here.
- **No `stat` accessor yet.** `FileInfo` is a Phase C placeholder with only a
  zero-init no-arg constructor; nothing populates it from disk. `Path.exists()`
  / `isFile()` / `isDir()` / `isSymlink()` are stubbed (return `false`) until
  Phase C lowering lands.
- **No charset decoding.** `FileReader.readString` takes bytes as UTF-8; transcode
  via `cajeta.io.Encoding` yourself.

## Inventory

**Entry-point types** (you call/instantiate these):
- `File` — both the static factory namespace (one-shots + `openRead`/`openWrite`/
  `open`/`openExclusive`) and the random-access instance handle (`read`/`write`/
  `seek`/`seekFromEnd`/`position`/`size`/`truncate`/`sync`/`flush`/`lock`/
  `tryLock`/`unlock`/`close`). See class skill `cajeta/io/file/File`.
- `Path` — immutable path; build with `Path.of(String) -> #Path`. Pure queries:
  `name`/`stem`/`extension`/`parent`/`isAbsolute`/`isRelative`/`resolve`. See
  `cajeta/io/file/Path`.
- `FileReader` / `FileWriter` — streaming handles, returned only by the `File`
  factories (internal-only ctors). See `cajeta/io/file/FileReader`,
  `cajeta/io/file/FileWriter`.

**Support types** (values/enums — do not instantiate as entry points):
- `OpenMode` enum — `READ, WRITE, APPEND, READ_WRITE, CREATE_NEW`. Ordinals are
  stable (the C runtime maps them to `O_*` flags); don't reorder.
- `FileInfo` — stat snapshot value (placeholder, see above).
- `FileEvent` / `WatchKind` — watch value + kind enum (deferred placeholders).

**Exceptions** — all extend `IoException`, which extends
`cajeta.error.RecoverableException` (catch the subtype or declare it in `throws`):
`NotFoundException`, `PermissionException`, `AlreadyExistsException`,
`IsDirectoryException`, `NotDirectoryException`, `EndOfFileException`,
`CrossDeviceException`, `DiskFullException`. Each maps an errno class (see
`docs/specification/io/file/Errors.md`).

## Collaboration

- `File.openRead(path)` mints a `#FileReader`; `File.openWrite(path, mode)` and
  the writer-mode factories mint a `#FileWriter`; both carry their own OS fd.
  You never construct `FileReader`/`FileWriter` directly — their ctors are
  internal and the factory binds the fd from the `open()` syscall.
- `OpenMode` flows into `File.open`/`File.openWrite` to declare intent.
- Path-string prep (`Path.of(...).resolve(...)`) feeds the `String path`
  argument the `File` factories take. `Path` and `File` do not otherwise share
  state.

## Ownership & lifecycle (package-wide)

- Every factory return marked `#` (`#int8[]`, `#FileReader`, `#FileWriter`,
  `#File`, `#Path`, `#String`) is **owned by the caller**; the drop chain frees
  it at scope exit.
- **Streaming reader/writer have NO auto-close-on-drop destructor in v1** — per
  the `FileReader`/`FileWriter` class docs you MUST call `close()` explicitly
  (it is idempotent, sets `fd = -1`). The random-access `File` handle's
  destructor does call `close()`, but closing explicitly is still the idiom.
- `Path.of(String)` **copies** the String's bytes into a fresh owned `int8[]`,
  so the source String can drop independently. The raw-bytes ctor
  `Path(int8[])` **takes** the bytes (`#` transfer).
- Errors are **exceptions**, not sentinels at the surface (the throwing wrappers
  are still being wired; today some intrinsics propagate `-1` internally — treat
  a `0` from `read` as EOF, not error).

## Worked example — streaming write then read

```cajeta
import cajeta.io.file.File;
import cajeta.io.file.FileReader;
import cajeta.io.file.FileWriter;
import cajeta.io.file.OpenMode;

// Write incrementally (WRITE truncates-or-creates).
FileWriter w = File.openWrite("/tmp/app.log", OpenMode.WRITE);
w.writeString("first line\n");
w.writeString("second line\n");
w.flush();
w.close();                       // required: no drop-close on the streaming handles

// Stream it back in fixed chunks until EOF (read == 0).
FileReader r = File.openRead("/tmp/app.log");
int8[] buf = heap int8[64];
int32 n = r.read(buf, 64);
while (n != 0) {
    // consume buf[0..n); r.position() tracks cumulative bytes
    n = r.read(buf, 64);
}
r.close();
```

For pure path manipulation with no I/O:

```cajeta
import cajeta.io.file.Path;

#Path p = Path.of("/foo/bar/baz.txt");
String ext = p.extension();              // "txt"
#Path child = p.parent().resolve("note.md");  // "/foo/bar/note.md"
```

## Pointers

Class-level depth: `cajeta/io/file/File`, `cajeta/io/file/Path`,
`cajeta/io/file/FileReader`, `cajeta/io/file/FileWriter`. Library map:
`cajeta.io`. Errno→exception chart: `docs/specification/io/file/Errors.md`.
