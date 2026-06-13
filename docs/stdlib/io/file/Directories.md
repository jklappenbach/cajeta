# Directory operations on `Path`

Directory create + mutation live as methods on `Path`. Two are
implemented today (`mkdirs`, `delete`); the streaming traversal and
copy/move surface is **planned** (Phase D) and documented here as
direction.

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

Not yet in the class — direction, not API:

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
(`cajeta.lang.stream.Stream`) so iteration composes with the
streams library (filter, take, count, …) without materializing the
whole tree.

## Notes

- **`mkdirs()`** — recursive (`mkdir -p`); idempotent (succeeds
  silently if the path already exists as a directory). Returns
  this `Path` for chaining. Intrinsic-lowered to
  `__cajeta_path_mkdirs`. A component that exists as a non-
  directory yields an error sentinel today; the throwing variant
  (`NotDirectoryException`, ENOTDIR) lands once the `IoException`
  hierarchy is wired.
- **`delete()`** — a single `unlink` / `rmdir`. Non-empty
  directories trip an error sentinel today; the recursive variant
  (`deleteRecursive`) arrives with the planned `Stream<Path>` walk
  primitives. Intrinsic-lowered to `__cajeta_path_delete`.
- **Planned — DFS vs BFS** — `walk()` would be depth-first;
  `bfs()` breadth-first. Pick the entry point (not chained).
- **Planned — glob syntax** — `*` matches within one segment,
  `**` spans segments; `?` one byte; `[abc]` / `[!a-z]` character
  classes; backslash escapes a metacharacter.
- **Planned — `copyTo` / `moveTo`** — copy byte-wise; move via
  `rename()` on the same device, else copy+delete. Cross-device
  renames surface `CrossDeviceException` (see
  [`Errors.md`](Errors.md)).

## See also

- [`Path.md`](Path.md) — base path methods.
- [`Watcher.md`](Watcher.md) — observe directory changes (planned).
- [`Errors.md`](Errors.md) — the exception hierarchy.
