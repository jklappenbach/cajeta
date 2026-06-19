# `cajeta.io.file.Path` — immutable filesystem path

`Path` is the immutable type that names a filesystem location. One
type — not split into `AbsolutePath` / `RelativePath`. It wraps the
raw OS-path bytes (`int8[]`) and offers pure-path queries (name,
stem, extension, parent, isAbsolute) plus join/resolve, all
syscall-free. Stat-touching methods (`exists`, `isFile`, `isDir`,
`isSymlink`, `canonical`) and the directory mutators (`mkdirs`,
`delete`) touch the filesystem.

## Surface (implemented)

```cajeta
public class Path {
    public int8[] bytes;                   // raw OS-path bytes, owned

    // --- Construction ---
    public Path(int8[] bytes);             // takes ownership of bytes
    public static #Path of(String s);

    // --- Joining ---
    public #Path resolve(String segment);  // append one segment with '/'

    // --- Decomposition (pure, never throw) ---
    public #Path  parent();
    public #String name();                 // last segment, with extension
    public #String stem();                 // last segment, no extension
    public #String extension();            // "archive.tar.gz" → "gz"
    public boolean isAbsolute();
    public boolean isRelative();

    // --- Single-stat predicates ---
    public boolean exists();
    public boolean isFile();
    public boolean isDir();
    public boolean isSymlink();

    // --- Normalization (syscall) ---
    public #Path canonical();              // realpath(3); resolves symlinks

    // --- Directory mutation (see Directories.md) ---
    public Path mkdirs();                  // recursive; idempotent
    public void delete();                  // file or empty dir
}
```

## Planned

Not yet in the class — documented as direction, not API:

```cajeta
public static Path of(String... parts);   // varargs join
public static Path of(int8[] os_bytes);    // static; today it's the ctor
public static Path cwd();
public static Path home();
public static Path tempDir();
public Path operator/(String segment);     // resolve() is the named form
public Path operator/(Path other);
public String[] parts();
public Path absolute();
public Path normalize();                   // collapse "." / ".."
public Path relativeTo(Path base);
public FileInfo info();                    // batched stat
```

## Notes

- **Construction** — `Path.of(String)` copies the String's bytes
  into a fresh owned `int8[]` (UTF-8 on Linux/mac — the String's
  bytes *are* the path bytes). The `Path(int8[])` constructor takes
  ownership of already-encoded OS bytes for callers that have a
  wire path; transfer with `#` (`heap Path(#raw)`).
- **Joining** — `resolve(segment)` appends one logical step with a
  `/` separator (no double slash if the path already ends in `/`,
  no leading slash if the path is empty). The segment is taken
  whole — embedded separators are NOT split (matches Python's
  pathlib). This is the named surface for the spec's `operator/`,
  which is planned once the grammar accepts a `#`-prefixed return
  type on operator methods.
- **Decomposition** — `name()` is the final segment with extension;
  `stem()` strips a single trailing extension
  (`archive.tar.gz` → `archive.tar`); `extension()` returns the
  text after the *rightmost* dot (`archive.tar.gz` → `gz`,
  `README` → `""`). A leading-dot dotfile with no further extension
  (`.bashrc`) has extension `""`.
- **Stat predicates** — `exists` / `isFile` / `isDir` / `isSymlink`
  each cost one `stat()` and return `false` on error today (no
  throw). The batched `info()` accessor that caches every attribute
  from one syscall is planned (see [`FileInfo.md`](FileInfo.md)).
- **`canonical()`** — POSIX `realpath(3)`; returns a fresh owned
  `#Path`. On a hard error it currently returns a `Path` wrapping a
  zero-length bytes array; the throwing variant lands once the
  `IoException` hierarchy is wired.

## Path encoding (v1)

Linux semantics: the underlying representation is `int8[]` — the
exact bytes a syscall sees. `Path.of(String)` takes the String's
UTF-8 octets directly. Windows-side normalization (`/` ↔ `\`,
drive letters, UNC) is a follow-up — Linux is the v1 target.

## See also

- [`FileInfo.md`](FileInfo.md) — the batched stat result (planned).
- [`Directories.md`](Directories.md) — `mkdirs` / `delete` (live);
  children / walk / glob / copy / move (planned).
- [`File.md`](File.md) — opens a path for read/write.
