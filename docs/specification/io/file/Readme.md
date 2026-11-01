# `cajeta.io.file` — package overview

File I/O is modeled on Python's `pathlib`, Rust's `Path` and Go's
`os.ReadFile`. Common cases are one line, and edge cases do not
fight you. Capability gating is planned and not yet implemented.
Under it, every read/write/list/watch method carries
`@capability("filesystem")`, so a program that does not declare the
capability fails at compile time (see `BuildTool.md` and
[`Errors.md`](Errors.md)).

Phases A, B and E are implemented and tested. Phase C is partial.
Phase D and the Watcher / async / capability surface are planned.
Concretely, the streaming `FileReader` / `FileWriter`, the one-shot
`File.readAllBytes` / `writeAllBytes`, the random-access `File`
handle, `OpenMode`, and the pure-path `Path` queries plus the stat
predicates and `mkdirs` / `delete` are live. The exception
hierarchy classes exist but are not yet thrown from the I/O paths,
which return sentinels today (see [`Errors.md`](Errors.md)).

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
   case, and the streaming `FileReader` / `FileWriter` cover the
   rest. `Path` is the immutable type that names a filesystem
   location.
2. **Destructor-based close (planned for the I/O classes).**
   Cajeta's destructor chain (locked 2026-05-18 in `MemoryModel.md`)
   fires `~ClassName()` at scope exit deterministically. There is
   no try-with-resources, no `with` and no `defer`.
   Try-with-resources was removed from the grammar 2026-05-20 as
   strictly redundant (see § "Resource cleanup is the destructor's
   job" below). Note that `File` / `FileReader` / `FileWriter` do
   not yet declare a `~` destructor, so today callers must
   `close()` explicitly. The auto-close-on-drop wiring is planned,
   and `close()` is idempotent.
3. **Bytes-first.** The streaming surface talks `int8[]`. Text
   decoding lives in a separate layer (cajeta.lang.String), and
   `FileReader` itself stays encoding-agnostic. Java's
   "FileReader uses platform charset" trap does not exist here,
   because text decoding is not part of the file API at all.
4. **Enum mode, not strings.** The modes are `OpenMode.READ` /
   `WRITE` / `APPEND` / `READ_WRITE` / `CREATE_NEW`. There are no
   mode strings such as `"rb+"`.
5. **No caller-visible buffering ceremony.** There is no
   user-space buffer. The caller's `int8[]` is passed straight to
   the syscall, and the runtime loops past partial OS reads and
   writes. There is no `BufReader::new(file)` ceremony, and
   `flush()` is a no-op because there is nothing to drain.
6. **Partial-transfer hiding.** Partial OS reads and writes (a
   `read()` / `write()` returning less than requested) are hidden
   inside the runtime loop. A streaming `read` fills the request
   fully or stops at EOF, which is signaled by a `0` return. The
   hard "all-or-error" `EndOfFileException` form is a planned
   Tier-2 helper and is not the default streaming behavior.

## Prior-art survey

This survey records what to copy and what to skip.

- **Python** has `open(path, mode)` + `with` context manager.
  Steal the one entry point and the deterministic close. Drop the
  stringly-typed mode flags (`'rb+'` typos at runtime) and the
  text-vs-binary surface gotcha.
- **Rust** has `std::fs::File::open()` / `File::create()` for
  streams, `fs::read()` / `read_to_string()` / `write()` for
  one-liners, and `BufReader::new(file)` for buffering. Steal the
  type-safe `Result<T, io::Error>` (we substitute exceptions) and
  the clean separation of "all the bytes" from "stream". Drop the
  explicit buffering wrap, which is ceremony for the common case.
- **Go** has `os.ReadFile(path)` / `os.WriteFile(path, data)`,
  `os.Open()` + `defer f.Close()`, and `bufio.Scanner` for lines.
  Steal the extremely small surface, mostly one-liners. Drop the
  multi-return-error verbosity, which exceptions solve.
- **C# .NET** has `File.ReadAllText` / `File.ReadAllBytes` static
  shortcuts and `StreamReader` / `StreamWriter` with explicit
  encoding. Steal the clean static-shortcut surface. Drop the deep
  inheritance hierarchy.
- **Java** shows mostly what NOT to do. It has three parallel
  hierarchies (`Reader` / `Writer`, `InputStream` / `OutputStream`,
  `java.nio.file.Files`), a `FileReader` that uses the
  platform-default charset (a data-corruption magnet), and
  checked-exception noise.

## What to avoid

- **Charset bleed-through.** Java's `FileReader` uses the
  platform-default encoding, and that is a bug magnet.
- **Multiple parallel hierarchies.** Java splits Reader /
  InputStream / Files. We have one factory and two streaming
  classes.
- **Silent partial reads / writes.** OS calls can return short.
  The reader and writer loop internally until satisfied or
  throw.
- **Lazy cleanup** (Java `finalize`). Cleanup is the destructor on
  scope exit.
- **Splitting line iteration into a separate class** (Java's
  `BufferedReader.readLine`). Lines are a Tier-2 concern. Ship
  the byte primitives, and layer `cajeta.io.file.Lines` (or
  `LineReader`) on top later.
- **Mode strings.** There is no `'r+b'`.

## Path encoding (v1 decision)

The encoding follows Linux semantics. `Path` ultimately wraps a raw
byte sequence, and the underlying `int8[]` is the wire layer the
syscall sees. `Path.of(String)` translates UTF-8 (Linux/mac) or
UTF-16 (Windows) at the boundary. macOS / Windows path normalization
is a follow-up, and the byte representation is the lingua franca.

## Implementation phases

- **Phase A (done)** covers `File.readAllBytes` / `writeAllBytes`,
  `OpenMode`, `FileReader` and `FileWriter`, with JSON file I/O
  end-to-end.
- **Phase B (done)** covers `Path` pure path manipulation
  (`of` / `name` / `stem` / `extension` / `parent` / `resolve` /
  `isAbsolute` / `isRelative`).
- **Phase C (partial).** The stat-touching `Path` predicates
  (`exists` / `isFile` / `isDir` / `isSymlink`) and `canonical` are
  live. The `IoException` hierarchy classes exist but are not yet
  thrown from the I/O paths, and `FileInfo` is a zero-init
  placeholder with no `Path.info()` accessor wired.
- **Phase D (partial).** `Path.mkdirs` / `delete` are live.
  `copyTo` / `moveTo` / `deleteRecursive` and the streaming
  `children` / `walk` / `glob` (returning `Stream<Path>`) are
  planned.
- **Phase E (done)** covers the random-access `File` (seek /
  position / size / truncate / sync / lock).
- **Deferred** are `Watcher`, the `*Async` forms, capability
  gating enforcement, `Path`/`Buffer` arg overloads, and the
  `InputStream` / `OutputStream` interfaces.

## Resource cleanup is the destructor's job

`try (FileReader r = …) { … }` (try-with-resources) was removed
from the grammar on 2026-05-20. Cajeta's destructor chain fires
the resource class's `~ClassName()` at the closing `}` of the
declaring block deterministically, in LIFO order across multiple
locals, including on exception unwind. The Java-style
try-with-resources syntax was strictly redundant. It existed in
Java only because Java has GC and no destructors, so `close()`
needed a separate guarantee mechanism. Cajeta already has the
better mechanism, so the syntax is gone.

This is the language-level model. The I/O classes are not yet
wired into it. `File` / `FileReader` / `FileWriter` do not
currently declare a `~ClassName()` destructor, so the auto-close
shown below is the planned end state. Until those destructors land,
call `close()` explicitly. The replacement pattern is to declare
the resource and close it:

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

`close()` is idempotent. It sets `this.fd == -1` so a second call
(or the planned destructor) skips the syscall. That idempotent-close
contract holds today for `FileReader` / `FileWriter` / `File`. The
drop chain that would call it automatically on every exit path
(exception unwind, return, throw) is the planned wiring.

Once the destructors land, a scope narrower than the method comes
for free. Open a fresh `{ … }` block and the resource drops at its
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

For the rationale and the Java-comparison details, see
`docs/MemoryModel.md` § Destructors.
