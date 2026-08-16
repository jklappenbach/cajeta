# `cajeta.io.file` — package overview

File I/O modeled on Python's `pathlib` + Rust's `Path` + Go's
`os.ReadFile`. Common cases are one line; edge cases don't fight
you. Capability gating — every read/write/list/watch method carrying
`@capability("filesystem")` so a program that doesn't declare the
capability fails at compile time — is *planned*, not yet
implemented (see `BuildTool.md` and [`Errors.md`](Errors.md)).

Status: **Phases A, B, and E implemented and tested; Phase C
partial; Phase D and the Watcher / async / capability surface
planned.** Concretely: the streaming `FileReader` / `FileWriter`,
the one-shot `File.readAllBytes` / `writeAllBytes`, the
random-access `File` handle, `OpenMode`, and pure-path `Path`
queries plus the stat predicates and `mkdirs` / `delete` are live.
The exception hierarchy classes exist but are not yet thrown from
the I/O paths (sentinel returns today — see [`Errors.md`](Errors.md)).

## File layout

| File                                                | Topic                                                      |
|-----------------------------------------------------|------------------------------------------------------------|
| [`Path.md`](Path.md)                                | Immutable path type — construction, joining, decomposition, single-stat predicates, `mkdirs` / `delete`. |
| [`FileInfo.md`](FileInfo.md)                        | Batched stat result (Phase C placeholder; no `Path` accessor wired yet). |
| [`File.md`](File.md)                                | File class — one-shot statics + random-access handle.      |
| [`OpenMode.md`](OpenMode.md)                        | Enum of file-open intents.                                 |
| [`FileReader.md`](FileReader.md)                    | Streaming reader — byte-loop / `readString` API.           |
| [`FileWriter.md`](FileWriter.md)                    | Streaming writer — `write` / `writeString` / `close`.      |
| [`Directories.md`](Directories.md)                  | Directory ops on `Path` — `mkdirs` / `delete` (live); walk / glob / copy / move (planned). |
| [`Watcher.md`](Watcher.md)                          | Filesystem notifications — Watcher + FileEvent + WatchKind (deferred). |
| [`Errors.md`](Errors.md)                            | IoException hierarchy + (planned) capability gating + async forms. |

## Design tenets

1. **One namespace, two patterns.** Static one-shots
   (`File.readAllBytes`, `File.writeAllBytes`) cover the common
   case; streaming `FileReader` / `FileWriter` cover the rest.
   `Path` is the immutable type that names a filesystem location.
2. **Destructor-based close (planned for the I/O classes).**
   Cajeta's destructor chain (locked 2026-05-18 in `MemoryModel.md`)
   fires `~ClassName()` at scope exit deterministically. No
   try-with-resources (removed from the grammar 2026-05-20 as
   strictly redundant — see § "Resource cleanup is the destructor's
   job" below), no `with`, no `defer`. **Note:** `File` /
   `FileReader` / `FileWriter` do not yet declare a `~` destructor,
   so today callers must `close()` explicitly; the auto-close-on-drop
   wiring is planned. `close()` is idempotent.
3. **Bytes-first.** The streaming surface talks `int8[]`. Text
   decoding lives in a separate layer (cajeta.lang.String);
   `FileReader` itself stays encoding-agnostic. Java's
   "FileReader uses platform charset" trap doesn't exist here
   because text decoding isn't part of the file API at all.
4. **Enum mode, not strings.** `OpenMode.READ` / `WRITE` /
   `APPEND` / `READ_WRITE` / `CREATE_NEW`. No mode strings
   (`"rb+"`).
5. **No caller-visible buffering ceremony.** There is no
   user-space buffer: the caller's `int8[]` is passed straight to
   the syscall, and the runtime loops past partial OS reads/writes.
   No `BufReader::new(file)` ceremony, and `flush()` is a no-op
   (nothing to drain).
6. **Partial-transfer hiding.** Partial OS reads/writes (a
   `read()` / `write()` returning less than requested) are hidden
   inside the runtime loop — a streaming `read` fills the request
   fully or stops at EOF (signaled by a `0` return). The hard
   "all-or-error" `EndOfFileException` form is a Tier-2 helper
   (planned), not the default streaming behavior.

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

- **Phase A (done)** — `File.readAllBytes` / `writeAllBytes`,
  `OpenMode`, `FileReader`, `FileWriter`. JSON file I/O end-to-end.
- **Phase B (done)** — `Path` pure path manipulation
  (`of` / `name` / `stem` / `extension` / `parent` / `resolve` /
  `isAbsolute` / `isRelative`).
- **Phase C (partial)** — stat-touching `Path` predicates
  (`exists` / `isFile` / `isDir` / `isSymlink`) and `canonical` are
  live; the `IoException` hierarchy classes exist but are not yet
  thrown from the I/O paths; `FileInfo` is a zero-init placeholder
  with no `Path.info()` accessor wired.
- **Phase D (partial)** — `Path.mkdirs` / `delete` are live;
  `copyTo` / `moveTo` / `deleteRecursive` and the streaming
  `children` / `walk` / `glob` (returning `Stream<Path>`) are
  planned.
- **Phase E (done)** — random-access `File` (seek / position /
  size / truncate / sync / lock).
- **Deferred** — `Watcher`, `*Async` forms, capability gating
  enforcement, `Path`/`Buffer` arg overloads, `InputStream` /
  `OutputStream` interfaces.

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

This is the language-level model. **The I/O classes are not yet
wired into it** — `File` / `FileReader` / `FileWriter` do not
currently declare a `~ClassName()` destructor, so the auto-close
shown below is the planned end state; until those destructors land,
call `close()` explicitly. The replacement pattern is "just declare
the resource and close it":

```cajeta
{
    FileReader r #= File.openRead(in);
    FileWriter w #= File.openWrite(out, OpenMode.WRITE);
    int32 n = r.read(buf, 4096);
    while (n > 0) {
        w.write(buf, n);
        n = r.read(buf, 4096);
    }
    w.close();   // explicit today; planned: ~FileWriter() flush + close
    r.close();   // explicit today; planned: ~FileReader() close
    // Planned LIFO drop order: w then r, guaranteed even on unwind.
}
```

`close()` is idempotent — it sets `this.fd == -1` so a second call
(or the planned destructor) skips the syscall. That idempotent-close
contract holds today for `FileReader` / `FileWriter` / `File`; the
drop-chain that would call it automatically on every exit path
(exception unwind, return, throw) is the planned wiring.

Once the destructors land, narrower-than-method scope comes for
free — open a fresh `{ … }` block and the resource drops at its
closing brace:

```cajeta
public static void process() {
    setup();
    {
        FileReader r #= File.openRead(in);
        consume(r);
        r.close();   // explicit today; planned: drops at the `}` below
    }  // r drops here, BEFORE the rest of process().
    rest();
}
```

For the rationale + Java-comparison details, see
`docs/MemoryModel.md` § Destructors.
