# Directory operations on `Path`

Directory creation and mutation live as methods on `Path`. Two are
implemented today, `mkdirs` and `delete`. The streaming traversal
and the copy/move surface are planned for Phase D, and they are
documented here as direction.

## Surface (implemented)

```cajeta
public class Path {
    public Path mkdirs();                          // recursive; idempotent
    public void delete();                          // file or empty dir
}
```

```cajeta
Path.of("a/b/c").mkdirs();        // creates every missing level
Path.of("a.txt").delete();        // unlink a file / rmdir an empty dir
```

## Surface (planned)

These methods are not yet in the class. They are direction rather
than API:

```cajeta
public class Path {
    public Path copyTo(Path target);
    public Path moveTo(Path target);
    public void deleteRecursive();                 // dir tree

    // Streaming traversal — Stream<Path>
    public Stream<Path> children();                // one level
    public Stream<Path> walk();                    // DFS by default
    public Stream<Path> bfs();                     // walk variant
    public Stream<Path> glob(String pattern);
}
```

The planned traversal methods return `Stream<Path>`
(`cajeta.lang.stream.Stream`), so iteration composes with the
streams library (filter, take, count, …) without materializing the
whole tree.

## Notes

- **`mkdirs()`** is recursive (`mkdir -p`) and idempotent, so it
  succeeds silently if the path already exists as a directory. It
  returns this `Path` for chaining, and it is intrinsic-lowered to
  `__cajeta_path_mkdirs`. A component that exists as a non-
  directory yields an error sentinel today. The throwing variant
  (`NotDirectoryException`, ENOTDIR) lands once the `IoException`
  hierarchy is wired.
- **`delete()`** is a single `unlink` / `rmdir`. Non-empty
  directories trip an error sentinel today. The recursive variant
  (`deleteRecursive`) arrives with the planned `Stream<Path>` walk
  primitives. It is intrinsic-lowered to `__cajeta_path_delete`.
- **Planned — DFS vs BFS.** `walk()` would be depth-first and
  `bfs()` breadth-first. You pick the entry point, and the two are
  not chained.
- **Planned — glob syntax.** `*` matches within one segment and
  `**` spans segments. `?` matches one byte, `[abc]` / `[!a-z]` are
  character classes, and a backslash escapes a metacharacter.
- **Planned — `copyTo` / `moveTo`.** Copy is byte-wise. Move uses
  `rename()` on the same device and copy+delete otherwise.
  Cross-device renames surface `CrossDeviceException` (see
  [`Errors.md`](Errors.md)).

## See also

- [`Path.md`](Path.md) — base path methods.
- [`Watcher.md`](Watcher.md) — observe directory changes (planned).
- [`Errors.md`](Errors.md) — the exception hierarchy.
