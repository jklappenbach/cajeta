# `cajeta.io.file` — package overview

File I/O modeled on Python's `pathlib` + Rust's `Path` + Go's
`os.ReadFile`. Common cases are one line; edge cases don't fight
you. Capability-gated: every read/write/list/watch method carries
`@capability("filesystem")` so a program that doesn't declare the
capability fails at compile time (see `BuildTool.md`).

Status: **design-phase complete; Phase A implementation tracked
under cajeta-docs Features.md / ToDo.md.** The class spec in
adjacent files is what the implementation builds toward; signatures
are stable, bodies land per phase.

## File layout

| File                                                | Topic                                                      |
|-----------------------------------------------------|------------------------------------------------------------|
| [`Path.md`](Path.md)                                | Immutable path value type — construction, joining, decomposition, normalization, single-stat predicates. |
| [`FileInfo.md`](FileInfo.md)                        | Batched stat result returned by `Path.info()`.             |
| [`File.md`](File.md)                                | File class — one-shot statics + random-access handle.      |
| [`OpenMode.md`](OpenMode.md)                        | Enum of file-open intents.                                 |
| [`FileReader.md`](FileReader.md)                    | Streaming reader — 8 KiB internal buffer, byte-loop API.   |
| [`FileWriter.md`](FileWriter.md)                    | Streaming writer — 8 KiB internal buffer, flush/close.     |
| [`Directories.md`](Directories.md)                  | Directory walk / glob / mkdirs / copyTo / moveTo / delete. |
| [`Watcher.md`](Watcher.md)                          | Filesystem notifications — Watcher + FileEvent + WatchKind. |
| [`Errors.md`](Errors.md)                            | IoException hierarchy + capability gating + async forms.   |

## Design tenets

1. **One namespace, two patterns.** Static one-shots
   (`File.readAllBytes`, `File.writeAllBytes`) cover the common
   case; streaming `FileReader` / `FileWriter` cover the rest.
   `Path` is the value type that names a filesystem location.
2. **Destructor-based close.** Cajeta's destructor chain (locked
   2026-05-18 in `MemoryModel.md`) fires at scope exit
   deterministically. No try-with-resources (removed from the
   grammar 2026-05-20 as strictly redundant — see § "Resource
   cleanup is the destructor's job" below), no `with`, no
   `defer`. Callers can still `close()` explicitly for early
   release.
3. **Bytes-first.** The streaming surface talks `int8[]`. Text
   decoding lives in a separate layer (cajeta.lang.String);
   `FileReader` itself stays encoding-agnostic. Java's
   "FileReader uses platform charset" trap doesn't exist here
   because text decoding isn't part of the file API at all.
4. **Enum mode, not strings.** `OpenMode.READ` / `WRITE` /
   `APPEND` / `READ_WRITE` / `CREATE_NEW`. No mode strings
   (`"rb+"`).
5. **Internal buffering, always.** The 8 KiB chunk is hidden
   inside the reader/writer — caller doesn't think about it.
   No `BufReader::new(file)` ceremony.
6. **All-or-error semantics.** Partial reads (OS `read()`
   returning less than requested) are hidden inside the
   reader's loop. The caller asks for N bytes, gets N or an
   `EndOfFileException`.

## Prior-art survey

What to copy, what to skip.

- **Python** — `open(path, mode)` + `with` context manager.
  Steal: one entry point, deterministic close. Drop: stringly-
  typed mode flags (`'rb+'` typos at runtime); text-vs-binary
  surface gotcha.
- **Rust** — `std::fs::File::open()` / `File::create()` for
  streams, `fs::read()` / `read_to_string()` / `write()` for
  one-liners, `BufReader::new(file)` for buffering. Steal:
  type-safe `Result<T, io::Error>` (we substitute exceptions);
  clean separation of "all the bytes" from "stream". Drop:
  explicit buffering wrap is ceremony for the common case.
- **Go** — `os.ReadFile(path)` / `os.WriteFile(path, data)`,
  `os.Open()` + `defer f.Close()`, `bufio.Scanner` for lines.
  Steal: extremely small surface, mostly one-liners. Drop:
  multi-return-error verbosity (solved by exceptions).
- **C# .NET** — `File.ReadAllText` / `File.ReadAllBytes` static
  shortcuts, `StreamReader` / `StreamWriter` with explicit
  encoding. Steal: clean static-shortcut surface. Drop: deep
  inheritance hierarchy.
- **Java** — what NOT to do, mostly. Three parallel hierarchies
  (`Reader` / `Writer`, `InputStream` / `OutputStream`,
  `java.nio.file.Files`), `FileReader` uses platform-default
  charset (data-corruption magnet), checked-exception noise.

## What to avoid

- **Charset bleed-through** (Java's `FileReader` — platform-
  default encoding bug magnet).
- **Multiple parallel hierarchies** (Java's Reader / InputStream
  / Files split). One factory, two streaming classes.
- **Silent partial reads / writes.** OS calls can return short.
  The reader / writer loops internally until satisfied or
  throws.
- **Lazy cleanup** (Java `finalize`). Destructor on scope exit,
  period.
- **Splitting line iteration into a separate class** (Java's
  `BufferedReader.readLine`). Lines are a Tier-2 concern; ship
  the byte primitives, layer `cajeta.io.file.Lines` (or
  `LineReader`) on top later.
- **Mode strings.** No `'r+b'`.

## Path encoding (v1 decision)

Linux semantics: `Path` ultimately wraps a raw byte sequence. The
underlying `int8[]` is the wire layer the syscall sees.
`Path.of(String)` translates UTF-8 (Linux/mac) or UTF-16 (Windows)
at the boundary. macOS / Windows path normalization is a follow-
up — the byte representation is the lingua franca.

## Implementation phases

- **Phase A** — `File.readAllBytes` / `writeAllBytes`,
  `OpenMode`, `FileReader`, `FileWriter`. JSON file I/O end-to-
  end.
- **Phase B** — `Path` (pure path manipulation; stat-touching
  methods deferred to C).
- **Phase C** — `FileInfo`, stat-touching `Path` methods,
  `IoException` hierarchy.
- **Phase D** — `Directories` (mkdirs, copyTo, moveTo, delete,
  children, walk, glob).
- **Phase E** — random-access `File` (seek / lock).
- **Deferred** — `Watcher`, `*Async` forms, capability gating
  enforcement.

## Resource cleanup is the destructor's job

`try (FileReader r = …) { … }` (try-with-resources) was removed
from the grammar on 2026-05-20. Cajeta's destructor chain fires
the resource class's `~ClassName()` at the closing `}` of the
declaring block deterministically, in LIFO order across multiple
locals, including on exception unwind. The Java-style
try-with-resources syntax was strictly redundant — it existed in
Java only because Java has GC and no destructors, so
`close()` needed a separate guarantee-mechanism. Cajeta
already has the better mechanism, so the syntax is gone.

The replacement pattern is "just declare the resource":

```cajeta
{
    FileReader r = File.openRead(in);
    FileWriter w = File.openWrite(out, OpenMode.WRITE);
    int32 n = r.read(buf, 4096);
    while (n > 0) {
        w.write(buf, n);
        n = r.read(buf, 4096);
    }
    // w.~FileWriter() fires here (flush + close), then
    // r.~FileReader() (close). LIFO order, guaranteed.
}
```

`r.close()` is still callable for early release — the destructor
sees `this.fd == -1` after the explicit close and skips the
syscall (idempotent close is the Phase-A contract for `FileReader`
/ `FileWriter` / `File`). Exception unwind, return, throw — every
exit path walks the drop chain back to the block's entry
watermark.

If you want narrower-than-method scope without an outer `{ }`
wrapper, just open a fresh `{ … }` block — same effect:

```cajeta
public static void process() {
    setup();
    {
        FileReader r = File.openRead(in);
        consume(r);
    }  // r drops here, BEFORE the rest of process().
    rest();
}
```

For the rationale + Java-comparison details, see
`cajeta-docs/MemoryModel.md` § Destructors.
