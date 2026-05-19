# `cajeta.io.file` — Path + File handle + Watcher

File I/O modeled on Python's `pathlib` + Rust's `Path` + Go's
`os.ReadFile`. Common cases one line; edge cases don't fight you.
Capability-gated: every read/write/list/watch method carries
`@capability("filesystem")` so a program that doesn't declare the
capability fails at compile time (see BuildTool.md).

Status: **designed, not implemented**. Tracked in Features.md.

## `Path` — value type, immutable

```cajeta
public final class Path {
    // Construction
    public static Path of(String s);
    public static Path of(String... parts);
    public static Path of(byte[] os_bytes);
    public static Path cwd();
    public static Path home();
    public static Path tempDir();

    // Joining
    public Path operator/(String segment);
    public Path operator/(Path other);
    public Path resolve(String segment);

    // Decomposition
    public Path parent();
    public String name();              // last segment, with extension
    public String stem();              // last segment, no extension
    public String extension();         // ".tar.gz" → "gz"
    public String[] parts();
    public boolean isAbsolute();
    public boolean isRelative();

    // Normalization
    public Path absolute();
    public Path canonical();           // resolves symlinks too
    public Path normalize();           // collapses "." / ".."
    public Path relativeTo(Path base);

    // Single-stat predicates
    public boolean exists();
    public boolean isFile();
    public boolean isDir();
    public boolean isSymlink();

    // Batched metadata
    public FileInfo info();
}
```

`Path` is one type, not split into `AbsolutePath` / `RelativePath`.

## `FileInfo` — batched stat result

```cajeta
public final value class FileInfo {
    public int64 size();
    public Instant created();
    public Instant modified();
    public Instant accessed();
    public boolean isFile();
    public boolean isDir();
    public boolean isSymlink();
    public int32 permissions();        // POSIX mode bits
}
```

## Reading and writing — one-liners

```cajeta
// Filesystem ops
Path.of("a.txt").exists();
Path.of("a.txt").copyTo(Path.of("b.txt"));
Path.of("a.txt").moveTo(Path.of("b.txt"));
Path.of("a.txt").delete();
```

Atomic writes: the implementation writes to `<name>.tmp.<rand>`,
fsyncs, then renames over the target (atomic on POSIX + NTFS).
`writeTextDirect` / `writeBytesDirect` opt out.

## `File` — handle for random access

```cajeta
public enum OpenMode { READ, WRITE, APPEND, READ_WRITE, CREATE_NEW }

public final class File implements InputStream, OutputStream {
    public static File open(Path p, OpenMode mode);
    public static File openExclusive(Path p);    // O_CREAT | O_EXCL

    public int64 read(Buffer dst);
    public int64 write(Buffer src);
    public int64 read(byte[] dst, int64 offset, int64 length);
    public int64 write(byte[] src, int64 offset, int64 length);

    public int64 position();
    public void  seek(int64 absolute);
    public void  seekFromEnd(int64 offset);
    public int64 size();
    public void  truncate(int64 size);
    public void  flush();
    public void  sync();                          // fsync
    public void  lock();
    public boolean tryLock();
    public void  unlock();

    public void  close();                         // also called by drop
}
```

Drops auto-close — explicit `close()` only for early release.

## Directories

```cajeta
// One level
for (Path child : Path.of(".").children()) { ... }

// Recursive
for (Path p : Path.of("src").walk()) { ... }            // DFS
for (Path p : Path.of("src").walk().bfs()) { ... }      // BFS

// Glob
for (Path p : Path.of("src").glob("**/*.cajeta")) { ... }

// Create
Path.of("a/b/c").mkdirs();
```

`children()` / `walk()` / `glob()` return `Stream<Path>` — they
stream, don't materialize.

## `Watcher`

```cajeta
public enum WatchKind { CREATE, MODIFY, DELETE, RENAME }

public final value class FileEvent {
    public Path path();
    public WatchKind kind();
    public Instant timestamp();
    public Path renameTarget();
}

public final class Watcher {
    public Stream<FileEvent> events();           // fiber-parks on read
    public void close();
}
```

Backed by `inotify` / `FSEvents` / `ReadDirectoryChangesW`.

## Async forms

Every blocking method has a `*Async` form returning `Task<T>`.

```cajeta
Task<String> t1 = Path.of("data.json").readTextAsync();
Task<void>   t2 = Path.of("out.txt").writeTextAsync("...");
```

## Errors

```cajeta
public class IoException             extends RecoverableException;
public class NotFoundException       extends IoException;
public class PermissionException     extends IoException;
public class AlreadyExistsException  extends IoException;
public class IsDirectoryException    extends IoException;
public class NotDirectoryException   extends IoException;
public class CrossDeviceException    extends IoException;
public class DiskFullException       extends IoException;
```

## Capability gating

Every I/O-effecting method carries `@capability("filesystem")`. Pure
path-manipulation (`parent`, `name`, `operator/`, `normalize`) is
NOT gated.

## FileReader / FileWriter — streaming siblings (2026-05-18 design pass)

The earlier `File` class above models random-access I/O (seek,
position, lock). The streaming case — "open this, slurp bytes
through it, close" — is much more common and deserves dedicated
classes with a smaller surface. Hence `FileReader` / `FileWriter`,
sitting beside `File` rather than replacing it.

### Sketch

```cajeta
public final class File {
    // One-shot shortcuts. Cover ~80% of read/write usage.
    public static #byte[] readAllBytes(Path p);
    public static void    writeAllBytes(Path p, byte[] data, int32 len);

    // Streaming opens. Caller drives the byte loop.
    public static #FileReader openRead(Path p);
    public static #FileWriter openWrite(Path p, OpenMode mode);
}

public class FileReader {
    public int32 read(byte[] buf, int32 max);   // bytes read; 0 == EOF
    public int64 position();
    public void  close();                        // optional; ~FileReader closes
    public ~FileReader();                        // closes if still open
}

public class FileWriter {
    public void  write(byte[] data, int32 len);
    public void  flush();
    public void  close();
    public ~FileWriter();                        // flushes + closes
}
```

The 8 KiB internal buffer is always present — callers don't pick
"buffered or unbuffered." No equivalent of Rust's `BufReader::new(file)`
wrapping ceremony.

### Prior-art survey (what to copy, what to skip)

- **Python** — `open(path, mode)` + `with` context manager. Steal:
  one entry point, deterministic close. Drop: stringly-typed mode
  flags (`'rb+'` typos at runtime); text-vs-binary surface gotcha.
- **Rust** — `std::fs::File::open()` / `File::create()` for streams,
  `fs::read()` / `read_to_string()` / `write()` for one-liners,
  `BufReader::new(file)` for buffering. Steal: type-safe
  `Result<T, io::Error>` (we substitute exceptions); clean
  separation of "all the bytes" from "stream." Drop: explicit
  buffering wrap is ceremony for the common case.
- **Go** — `os.ReadFile(path)` / `os.WriteFile(path, data)`,
  `os.Open()` + `defer f.Close()`, `bufio.Scanner` for lines.
  Steal: extremely small surface, mostly one-liners. Drop:
  multi-return-error verbosity (solved by exceptions).
- **C# .NET** — `File.ReadAllText` / `File.ReadAllBytes` static
  shortcuts, `StreamReader` / `StreamWriter` with explicit encoding.
  Steal: clean static-shortcut surface. Drop: deep inheritance
  hierarchy.
- **Java** — what NOT to do, mostly. Three parallel hierarchies
  (`Reader` / `Writer`, `InputStream` / `OutputStream`,
  `java.nio.file.Files`), `FileReader` uses platform-default
  charset (data-corruption magnet), checked-exception noise.

### Improvements over the prior art

1. **Destructor-based close.** Cajeta's destructor chain (locked
   2026-05-18 in `MemoryModel.md`) fires at scope exit deterministically.
   No try-with-resources, no `with`, no `defer`. Callers can still
   `close()` explicitly for early release.
2. **Bytes-first.** The streaming surface talks `byte[]` (the
   primitive Cajeta byte type). Text decoding lives in a separate
   `Text` / String-conversion layer; `FileReader` itself stays
   encoding-agnostic. Java's "FileReader uses platform charset"
   trap doesn't exist here because text decoding isn't part of the
   file API at all.
3. **Enum mode, not strings.** `OpenMode.READ / WRITE / APPEND /
   READ_WRITE / CREATE_NEW` (already in the random-access `File`
   above). Boolean flags for `truncate` / `exclusive` once needed.
4. **One-line shortcuts and streaming, both off `File`.**
   `File.readAllBytes(p)` and `File.writeAllBytes(p, bytes, len)`
   cover the common case; `File.openRead(p)` / `openWrite(p, mode)`
   return the streaming classes for the rest. One namespace, two
   patterns.
5. **Internal buffering, always.** The 8 KiB chunk is hidden inside
   the reader/writer — caller doesn't think about it.
6. **All-or-error semantics.** Partial reads (OS `read()` returning
   less than requested) are hidden inside the reader's loop. The
   caller asks for N bytes, gets N or an `EndOfFileException`.

### What to avoid

- **Charset bleed-through** (Java's `FileReader` — platform-default
  encoding bug magnet).
- **Multiple parallel hierarchies** (Java's Reader / InputStream /
  Files split). One factory, two streaming classes.
- **Silent partial reads / writes.** OS calls can return short. The
  reader / writer loops internally until satisfied or it throws.
- **Lazy cleanup** (Java `finalize`). Destructor on scope exit,
  period. No "we'll close it eventually."
- **Splitting line iteration into a separate class** (Java's
  `BufferedReader.readLine`). Lines are a Tier-2 concern; ship the
  byte primitives, layer `cajeta.io.file.Lines` (or
  `LineReader`) on top later.
- **Mode strings.** No `'r+b'`.

### Path encoding (v1 decision)

Linux semantics: `Path` ultimately wraps a raw byte sequence.
macOS / Windows path normalization deferred — the underlying
`byte[]` is the wire layer the syscall sees. Once a real `String`
class lands, `Path.of(String)` gains UTF-8 (Linux/mac) or UTF-16
(Windows) round-tripping as a thin layer above the bytes.

### Async

v1 is sync-only. The signatures above don't mention async; the
`*Async` forms (already in the random-access surface) can land
separately without breaking sync callers.

### Blocked on String

**The whole file API is blocked on a real `String` class.** Today
`String` is an opaque pointer alias (see CajetaType.cpp ~line 122)
— no `.length`, no comparison, no `.bytes()`, no construction from
`byte[]`. The `Path.of(String)` factory, `Path.name() / stem() /
extension()`, the error messages all need text manipulation. We
could ship `byte[]`-only file primitives now and layer the
`Path`-string surface on top once `String` is real, but the
ergonomic story is bad without it.

Plan: nail `String` down (see `cajeta-docs/stdlib/lang/String.md`
once it exists), THEN ship the streaming file primitives + Path
together so users see a coherent surface.

## Open items

All of `cajeta.io.file` is unimplemented. Tracked in Features.md.
