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
- **`CREATE_NEW`** is POSIX `O_EXCL`. It fails if the path exists,
  surfaced as an `EEXIST` open with a `-1` fd today and as
  `AlreadyExistsException` once the exception hierarchy is wired.
  This protects against TOCTOU races where two writers race past
  `if (!exists()) write(...)`. It pairs with `File.openExclusive`.
- **`APPEND`** is atomic at the syscall level on POSIX. The
  kernel positions every write at end-of-file regardless of
  interleaving with other appenders.
- **`READ_WRITE`** creates the file if missing and leaves
  existing contents alone. Pair it with an explicit `truncate(0)`
  if you want clobber-then-write semantics.
- **Extending the enum.** `TRUNCATE` and `EXCLUSIVE` flag
  variants are deferred. v1 keeps the five canonical opens.

## Where it's used

- [`File.openWrite`](File.md)`(path, mode)` /
  `File.open(path, mode)`. `File.openRead` is always read-only and
  takes no `OpenMode`.
- [`File.writeAllBytes`](File.md) uses `WRITE` semantics
  internally, with atomic-rename. The caller does not pass
  `OpenMode`.

## See also

- [`File.md`](File.md), [`FileReader.md`](FileReader.md),
  [`FileWriter.md`](FileWriter.md).
