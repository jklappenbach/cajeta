# Path

`cajeta.io.file.Path` — immutable filesystem path. Wraps the raw OS-path bytes
(UTF-8 octets, `/` separator on Linux) and offers pure-path queries (`name`,
`stem`, `extension`, `parent`, `isAbsolute`), stat predicates (`exists`,
`isFile`, `isDir`, `isSymlink`), join via `resolve`, and mutations (`mkdirs`,
`delete`, `symlinkTo`, `setExecutable`, `canonical`). Build one from a `String`
with the static factory `Path.of(...)`.

```cajeta
Path p = Path.of("/foo/bar/baz.txt");
String leaf = p.name();          // "baz.txt"
String base = p.stem();          // "baz"
String ext = p.extension();      // "txt"
Path dir = p.parent();           // "/foo/bar"
Path child = dir.resolve("note.md");
```

## Methods

| Signature | |
|---|---|
| `Path(int8[] bytes)` | Construct from already-encoded OS bytes (the Path takes them); most code uses `Path.of` |
| `static #Path of(String s)` ⚑ | Construct from a `String`, copying its bytes into a fresh owned array |
| `boolean isAbsolute()` | True if the path starts with `/` |
| `boolean isRelative()` | Negation of `isAbsolute()`; an empty path is relative |
| `#String name()` | Last segment, including extension |
| `#String stem()` | Last segment without its trailing extension (single-dot strip: `"archive.tar.gz"` → `"archive.tar"`) |
| `#String extension()` | Last extension after the rightmost dot (`""` when none; a leading-dot dotfile has no extension) |
| `#Path parent()` | Everything before the last separator (`"/foo"` → `"/"`; no separator → `""`) |
| `boolean exists()` | Stat predicate: the entry exists |
| `boolean isFile()` | Stat predicate: regular file |
| `boolean isDir()` | Stat predicate: directory |
| `boolean isSymlink()` | Stat predicate: symbolic link |
| `Path mkdirs()` | Create this path and every missing intermediate as directories (`mkdir -p`); idempotent, returns this for chaining |
| `void delete()` | Remove this filesystem entry (`unlink` / `rmdir`; non-empty directories are an error) |
| `boolean setExecutable()` | Add the executable bits (`chmod a+x`, preserving the other permission bits) |
| `boolean symlinkTo(Path target)` | Create this path as a symbolic link pointing at `target` (`ln -s target this`); replaces an existing entry |
| `#Path canonical()` | Resolved absolute path with symlinks walked (POSIX `realpath(3)`) |
| `#Path resolve(String segment)` | Append `segment` as a single path step, inserting `/` only when needed |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/file/Path.cajeta`](../../../../runtime/src/cajeta/io/file/Path.cajeta)
- [File](File.md) — open/read/write at a path; [FileInfo](FileInfo.md) — the multi-attribute stat snapshot; [Watcher](Watcher.md)
