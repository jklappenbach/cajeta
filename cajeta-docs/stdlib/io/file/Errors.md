# `cajeta.io.file` — exceptions + capability gating + async forms

Cross-cutting concerns for the file API: the exception hierarchy,
the capability annotation that gates every I/O method, and the
`*Async` forms that mirror the sync surface.

## Exception hierarchy

```cajeta
public class IoException             extends RecoverableException;
public class NotFoundException       extends IoException;
public class PermissionException     extends IoException;
public class AlreadyExistsException  extends IoException;
public class IsDirectoryException    extends IoException;
public class NotDirectoryException   extends IoException;
public class IsFileException         extends IoException;
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

### Recoverable, not Unrecoverable

`IoException` extends `RecoverableException` (see
`cajeta-docs/stdlib/ErrorModel.md`). Every IO-using call site
either:
- catches the relevant subtype explicitly, or
- declares it in the `throws` clause and propagates upward.

The compiler's uncaught-throws check (warning today, error
eventually) keeps the discipline visible.

## Capability gating

Every I/O-effecting method carries
`@capability("filesystem")`. A program that doesn't declare the
filesystem capability in its manifest fails at compile time —
see `cajeta-docs/BuildTool.md` for the manifest shape.

Pure path manipulation is NOT gated:

| Always allowed (no capability)              | Gated (`filesystem`)                    |
|---------------------------------------------|-----------------------------------------|
| `Path.of`, `parent`, `name`, `stem`,        | `Path.exists`, `isFile`, `info`,        |
| `extension`, `parts`, `operator/`,          | `canonical`, `children`, `walk`,        |
| `normalize`, `isAbsolute`, `isRelative`,    | `glob`, `mkdirs`, `copyTo`, `moveTo`,   |
| `relativeTo`                                | `delete`, all `File` / `FileReader` /   |
|                                             | `FileWriter` methods.                   |

## Async forms

Every blocking method has a `*Async` form returning `Task<T>`.

```cajeta
Task<#int8[]> t1 = File.readAllBytesAsync(p);
Task<void>    t2 = File.writeAllBytesAsync(p, data, len);
Task<FileInfo> t3 = p.infoAsync();
```

v1 ships sync-only. The async forms land after the async runtime
(`cajeta-docs/AsyncStatus.md`) gates them in — adding them
without breaking sync callers is mechanical (each `void
xxx(args)` gains a `Task<void> xxxAsync(args)` sibling that the
fiber scheduler runs on a worker pool).

## See also

- `cajeta-docs/stdlib/ErrorModel.md` — `Recoverable` /
  `Unrecoverable` hierarchy, throws-clause semantics.
- `cajeta-docs/BuildTool.md` — capability manifest format.
- `cajeta-docs/stdlib/AsyncStatus.md` — async runtime status.
