# `cajeta.io.file.Path` — immutable filesystem path

`Path` is the value type that names a filesystem location. One type
— not split into `AbsolutePath` / `RelativePath`. Construction and
manipulation methods (parts, name, parent, normalize, etc.) are
syscall-free and never throw; stat-touching methods (`exists`,
`isFile`, `info`, …) are documented separately at the bottom and
are capability-gated.

## Surface

```cajeta
public final class Path {
    // --- Construction ---
    public static Path of(String s);
    public static Path of(String... parts);
    public static Path of(int8[] os_bytes);
    public static Path cwd();
    public static Path home();
    public static Path tempDir();

    // --- Joining ---
    public Path operator/(String segment);
    public Path operator/(Path other);
    public Path resolve(String segment);

    // --- Decomposition ---
    public Path parent();
    public String name();              // last segment, with extension
    public String stem();              // last segment, no extension
    public String extension();         // ".tar.gz" → "gz"
    public String[] parts();
    public boolean isAbsolute();
    public boolean isRelative();

    // --- Normalization ---
    public Path absolute();
    public Path canonical();           // resolves symlinks too
    public Path normalize();           // collapses "." / ".."
    public Path relativeTo(Path base);

    // --- Single-stat predicates ---
    public boolean exists();
    public boolean isFile();
    public boolean isDir();
    public boolean isSymlink();

    // --- Batched metadata ---
    public FileInfo info();
}
```

## Notes

- **Construction** — `Path.of(String)` decodes UTF-8 (Linux/mac) /
  UTF-16 (Windows) at the boundary; `Path.of(int8[])` accepts the
  raw OS bytes for callers that already have a wire path.
- **Joining** — `operator/` is the canonical join. The right-hand
  side can be a String segment, another Path, or a list of
  segments (via repeated `/`). Embedded separators in a segment
  are NOT split — `Path.of("a") / "b/c"` produces a path with
  literal `b/c` as one segment (matches Python's pathlib).
- **Decomposition** — `name()` is the final segment with
  extension; `stem()` strips a single trailing extension;
  `extension()` returns the longest known multi-dot extension
  (`.tar.gz` → `"gz"`, single-dot stays). `parts()` returns the
  segments as a `String[]` in order.
- **Normalization** — `normalize()` is pure-syntactic;
  `canonical()` walks symlinks (capability-gated, syscall).
  `relativeTo(base)` throws when the receiver isn't under `base`.
- **Stat predicates** — `exists` / `isFile` / `isDir` /
  `isSymlink` each cost one `stat()`. For multi-question reads,
  call `info()` once and use the cached fields.

## Path encoding (v1)

Linux semantics: the underlying representation is `int8[]` — the
exact bytes a syscall sees. `Path.of(String)` encodes via UTF-8
on Linux/mac, UTF-16 on Windows. Windows-side normalization
(`/` ↔ `\`, drive letters, UNC) is a follow-up — Linux is the
v1 target.

## See also

- [`FileInfo.md`](FileInfo.md) — the batched stat result.
- [`Directories.md`](Directories.md) — children / walk / glob /
  mkdirs / copy / move / delete.
- [`File.md`](File.md) — opens a Path for read/write.
