# `cajeta.io.file.FileWriter` — streaming byte writer

Returned by `File.openWrite(p, mode)`. Internal 8 KiB buffer;
caller hands in bytes, the writer batches them and flushes on
demand / close.

## Surface

```cajeta
public class FileWriter {
    public void write(int8[] data, int32 len);
    public void flush();                         // drains the internal buffer
    public void close();                         // flush + close fd
    public ~FileWriter();                        // flush + close if still open
}
```

## Notes

- **`write(data, len)`** — writes `data[0..len)`. Partial OS
  writes are hidden by the internal loop — the call returns when
  every byte has been accepted by the kernel, or it throws on
  hard error (`DiskFullException`, etc.).
- **`flush()`** — drains the user-space buffer into the OS via
  `write()` syscalls. Does NOT `fsync` — durability flushing is
  via the random-access `File.sync()` (Phase E).
- **`close()` and the destructor** — both flush and close. The
  destructor is the safety net for early scope exit; explicit
  `close()` is only for releasing the fd before scope ends.
- **Buffer ownership** — `data` is the caller's array, copied
  into the writer's internal buffer; the writer never retains a
  reference.
- **No charset.** Bytes only.

## Idiomatic write

```cajeta
FileWriter w = File.openWrite(p, OpenMode.WRITE);
w.write(payload, payloadLen);
// flush + close happens on drop.
```

For the one-shot case (don't keep `w` around), see
`File.writeAllBytes(p, data, len)` — same effect, atomic-rename
semantics included.

## See also

- [`FileReader.md`](FileReader.md) — symmetric read side.
- [`OpenMode.md`](OpenMode.md) — `WRITE` / `APPEND` /
  `READ_WRITE` / `CREATE_NEW` modes accepted by the factory.
- [`File.md`](File.md) — one-shot `writeAllBytes`.
