# `cajeta.io.file.OpenMode` — file-open intent enum

```cajeta
public enum OpenMode {
    READ,           // O_RDONLY
    WRITE,          // O_WRONLY | O_CREAT | O_TRUNC
    APPEND,         // O_WRONLY | O_CREAT | O_APPEND
    READ_WRITE,     // O_RDWR   | O_CREAT
    CREATE_NEW      // O_WRONLY | O_CREAT | O_EXCL
}
```

## Notes

- **No mode strings.** Python's `"rb+"` typos at runtime.
  Cajeta picks the discrete-enum path so a typo is a
  compile-time identifier-not-found error.
- **`CREATE_NEW`** — POSIX `O_EXCL`. Fails (an `EEXIST` open,
  surfaced as a `-1` fd today; `AlreadyExistsException` once the
  exception hierarchy is wired) if the path exists; protects
  against TOCTOU races where two writers race past
  `if (!exists()) write(...)`. Pairs with `File.openExclusive`.
- **`APPEND`** — atomic at the syscall level on POSIX; the
  kernel positions every write at end-of-file regardless of
  interleaving with other appenders.
- **`READ_WRITE`** — creates the file if missing, leaves
  existing contents alone. Pair with explicit `truncate(0)` if
  you want clobber-then-write semantics.
- **Extending the enum** — `TRUNCATE` and `EXCLUSIVE` flag
  variants are deferred. v1 keeps the five canonical opens.

## Where it's used

- [`File.openWrite`](File.md)`(path, mode)` /
  `File.open(path, mode)`. (`File.openRead` is always read-only and
  takes no `OpenMode`.)
- [`File.writeAllBytes`](File.md) uses `WRITE` semantics
  internally (with atomic-rename); the caller doesn't pass
  `OpenMode`.

## See also

- [`File.md`](File.md), [`FileReader.md`](FileReader.md),
  [`FileWriter.md`](FileWriter.md).
