# FileInfo

`cajeta.io.file.FileInfo` — snapshot of a single `stat` call: size, timestamps,
type flags, and permission bits for one filesystem entry, captured in one
syscall so callers avoid per-attribute round-trips. Read the public fields
directly: `size`, `createdNanos` / `modifiedNanos` / `accessedNanos` (Unix
nanoseconds as `int64`), `isFile` / `isDir` / `isSymlink`, and `permissions`.
The flags mirror the cheaper single-attribute probes on [Path](Path.md); prefer
this type when you need several attributes at once. A stat-touching accessor on
`Path` that returns a populated `FileInfo` is planned; until then the only
constructor is the zero-initialized no-arg form.

```cajeta
FileInfo meta = heap FileInfo();
meta.size = (int64) 1024;
meta.isFile = true;
boolean regular = meta.isFile && !meta.isDir;
int64 mtime = meta.modifiedNanos;    // Unix nanoseconds
```

## Methods

| Signature | |
|---|---|
| `FileInfo()` ⚑ | Zero-initialized record (all sizes/timestamps `0`, all flags `false`) |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/file/FileInfo.cajeta`](../../../../runtime/src/cajeta/io/file/FileInfo.cajeta)
- [Path](Path.md) — the single-attribute stat probes (`exists` / `isFile` / `isDir` / `isSymlink`); [File](File.md)
