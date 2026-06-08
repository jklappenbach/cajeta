# Directory operations on `Path`

Phase D — directory walk + mutation. All capability-gated under
`@capability("filesystem")`. The streaming traversal methods
return `Stream<Path>` so callers can compose with the rest of
the streams library (filter, take, count, …).

## Surface

```cajeta
public class Path {
    // Mutation
    public Path mkdirs();                          // recursive; idempotent
    public Path copyTo(Path target);
    public Path moveTo(Path target);
    public void delete();                          // file or empty dir
    public void deleteRecursive();                 // dir tree

    // Traversal (streaming — Stream<Path>)
    public Stream<Path> children();                // one level
    public Stream<Path> walk();                    // DFS by default
    public Stream<Path> bfs();                     // walk variant
    public Stream<Path> glob(String pattern);
}
```

## Examples

```cajeta
// One level
for (Path child : Path.of(".").children()) { ... }

// Recursive
for (Path p : Path.of("src").walk()) { ... }       // DFS
for (Path p : Path.of("src").bfs()) { ... }        // BFS

// Glob
for (Path p : Path.of("src").glob("**/*.cajeta")) { ... }

// Create / mutate
Path.of("a/b/c").mkdirs();
Path.of("a.txt").copyTo(Path.of("b.txt"));
Path.of("a.txt").moveTo(Path.of("b.txt"));
Path.of("a.txt").delete();
```

## Notes

- **Streaming** — `children()` / `walk()` / `glob()` return
  `Stream<Path>`, so iteration doesn't materialize the whole
  tree up-front. Pair with `take(N)` for paginated walks.
- **DFS vs BFS** — `walk()` is depth-first (most common shape
  for "find this file"); `bfs()` is breadth-first (most common
  for "shallowest match"). `walk().bfs()` is NOT supported —
  pick the entry point.
- **Glob syntax** — `*` matches one segment, `**` matches any
  number of segments. `?` matches one byte; `[abc]` and `[!a-z]`
  match character classes within a segment. Backslash escapes a
  metacharacter.
- **`mkdirs()`** — recursive; idempotent. Throws
  `IsFileException` if any intermediate component exists as a
  file.
- **`copyTo` / `moveTo`** — copy is byte-wise on the source's
  filesystem; move is `rename()` on the same device, else
  copy+delete (atomicity not guaranteed across devices —
  `CrossDeviceException` thrown unless the caller opts in via
  `moveToOrCopy(target)`).
- **`delete()`** — fails with `IsDirectoryException` on a non-
  empty dir. Use `deleteRecursive()` for the tree-delete.

## See also

- [`Path.md`](Path.md) — base path methods.
- [`Watcher.md`](Watcher.md) — observe directory changes.
- [`Errors.md`](Errors.md) — the exception hierarchy.
