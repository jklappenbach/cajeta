# `cajeta.io.file.FileWriter` — streaming byte writer

Returned by `File.openWrite(path, mode)`. The caller hands in bytes
(or a `String`) per call; each write goes straight to the kernel —
there is no user-space buffer. The runtime loops past partial OS
writes so every requested byte lands before the call returns.

## Surface

```cajeta
public class FileWriter {
    public int32 fd;                             // OS fd, -1 once closed
    public int64 pos;                            // bytes accepted

    public void  write(int8[] data, int32 len);
    public void  writeString(String s);          // writes s's UTF-8 bytes
    public void  flush();                         // no-op (no user buffer)
    public void  close();                         // close fd; idempotent
}
```

## Notes

- **`write(data, len)`** — writes `data[0..len)`. Partial OS writes
  are hidden by the runtime loop — the call returns once every byte
  has been accepted by the kernel. On a hard error the native
  helper returns `-1`; a throwing wrapper
  (`DiskFullException`, etc.) is *planned* — see
  [`Errors.md`](Errors.md) — but not yet wired, so today errors do
  not surface as exceptions here.
- **`writeString(s)`** — writes the `String`'s UTF-8 bytes
  directly. No charset re-encode at this layer.
- **`flush()`** — a no-op today: there is no user-space buffer to
  drain, so writes are already in the kernel. It does NOT `fsync`
  — durability flushing is the random-access `File.sync()`
  (Phase E). Defined for stream conformance.
- **`close()`** — closes the fd; idempotent (sets `fd = -1`). The
  class does not yet declare a `~FileWriter()` destructor, so
  auto-close on scope exit is *planned*, not wired — call
  `close()` explicitly today.
- **Buffer ownership** — `data` is the caller's array; the writer
  never retains a reference past the call.
- **No charset.** Bytes only (and `writeString`'s pre-encoded
  UTF-8).

## Idiomatic write

```cajeta
FileWriter w = File.openWrite(path, OpenMode.WRITE);
w.write(payload, payloadLen);
w.close();   // explicit close (auto-close on drop is planned)
```

For the one-shot case (don't keep `w` around), see
`File.writeAllBytes(path, data, len)` — atomic-rename semantics
included.

## See also

- [`FileReader.md`](FileReader.md) — symmetric read side.
- [`OpenMode.md`](OpenMode.md) — `WRITE` / `APPEND` /
  `READ_WRITE` / `CREATE_NEW` modes accepted by the factory.
- [`File.md`](File.md) — one-shot `writeAllBytes`, the random-
  access `File.sync()`.
