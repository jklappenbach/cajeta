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
// Whole-file reads
String text = Path.of("data.json").readText();
byte[]  bin  = Path.of("model.bin").readBytes();

// Streaming reads — return Stream<T> for the for-each path
for (String line : Path.of("huge.log").readLines()) {
    process(line);
}
for (byte[] chunk : Path.of("video.mp4").readChunks(64 * 1024)) {
    pump(chunk);
}

// Whole-file writes — atomic by default
Path.of("output.txt").writeText("hello world");
Path.of("blob.bin").writeBytes(bytes);

// Appends — non-atomic by definition
Path.of("audit.log").appendText("entry\n");

// Filesystem ops
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

## Open items

All of `cajeta.io.file` is unimplemented. Tracked in Features.md.
