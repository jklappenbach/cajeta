# `cajeta.io.file` — exceptions + capability gating + async forms

Cross-cutting concerns for the file API: the exception hierarchy
(implemented), the capability annotation that is planned to gate
every I/O method, and the planned `*Async` forms that mirror the
sync surface.

## Exception hierarchy

All of these classes exist in `cajeta.io.file`. Each subtype's only
constructor takes a `String message`; the message is carried on the
inherited `Throwable.message` field and read as `e.message`.

```cajeta
public class IoException             extends RecoverableException;  // ctor(String)
public class NotFoundException       extends IoException;
public class PermissionException     extends IoException;
public class AlreadyExistsException  extends IoException;
public class IsDirectoryException    extends IoException;
public class NotDirectoryException   extends IoException;
public class CrossDeviceException    extends IoException;
public class DiskFullException       extends IoException;
public class EndOfFileException      extends IoException;
```

### Mapping from POSIX errno

| errno     | Exception              |
|-----------|------------------------|
| `ENOENT`  | `NotFoundException`    |
| `EACCES`  | `PermissionException`  |
| `EEXIST`  | `AlreadyExistsException` |
| `EISDIR`  | `IsDirectoryException` |
| `ENOTDIR` | `NotDirectoryException` |
| `EXDEV`   | `CrossDeviceException` |
| `ENOSPC`  | `DiskFullException`    |
| (eof)     | `EndOfFileException`   |
| (other)   | `IoException`          |

### Not yet thrown from the I/O paths

The hierarchy is **defined and catchable**, but the file/path
runtime helpers do not yet raise these exceptions — today they
return sentinels instead (`null` from `readAllBytes`, `-1` from
writes / instance reads, `false` from the stat predicates, an
empty-bytes `Path` from `canonical()`). The throwing wrappers that
map errno → the classes above land once the hierarchy is wired
end-to-end. The catch-side shape below is what those wrappers will
feed.

### Recoverable, not Unrecoverable

`IoException` extends `RecoverableException` (see
`docs/specification/error/ErrorModel.md`). The intended discipline at every
IO-using call site:
- catch the relevant subtype explicitly, or
- declare it in the `throws` clause and propagate upward.

```cajeta
try {
    FileReader r = File.openRead("/etc/config.toml");
    // ... read it
    r.close();
} catch (NotFoundException e) {
    // file isn't there — fall back to defaults
} catch (IoException e) {
    log.warn(e.message);
}
```

A compiler uncaught-throws check that keeps the discipline visible
is planned.

## Capability gating (planned)

The design intent is that every I/O-effecting method carries a
`@capability("filesystem")` annotation, so a program that doesn't
declare the filesystem capability in its manifest fails at compile
time. **This is not implemented** — no `@capability` annotation
exists in the codebase yet; treat the table below as the planned
split between always-allowed pure-path operations and gated
filesystem operations. See `docs/BuildTool.md` for the manifest
shape.

| Always allowed (planned: no capability)     | Planned: gated (`filesystem`)           |
|---------------------------------------------|-----------------------------------------|
| `Path.of`, `parent`, `name`, `stem`,        | `Path.exists`, `isFile`, `isDir`,       |
| `extension`, `isAbsolute`, `isRelative`,    | `isSymlink`, `canonical`, `mkdirs`,     |
| `resolve`                                   | `delete`, all `File` / `FileReader` /   |
|                                             | `FileWriter` methods.                   |

## Async forms (planned)

The plan is for every blocking method to gain a `*Async` form
returning `Task<T>`. **Not implemented** — there is no `Task` class
yet and the I/O API ships sync-only.

```cajeta
// Planned shape, not yet available:
Task<#int8[]>  t1 = File.readAllBytesAsync(path);
Task<void>     t2 = File.writeAllBytesAsync(path, data, len);
Task<FileInfo> t3 = p.infoAsync();
```

The async forms land after the async runtime
(`docs/specification/concurrent/AsyncStatus.md`) gates them in — adding them without
breaking sync callers is mechanical (each `void xxx(args)` gains a
`Task<void> xxxAsync(args)` sibling run on a worker pool).

## See also

- `docs/specification/error/ErrorModel.md` — `Recoverable` /
  `Unrecoverable` hierarchy, throws-clause semantics.
- `docs/BuildTool.md` — capability manifest format.
- `docs/specification/concurrent/AsyncStatus.md` — async runtime status.
