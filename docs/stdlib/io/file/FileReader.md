# `cajeta.io.file.FileReader` — streaming byte reader

Returned by `File.openRead(p)`. Internal 8 KiB buffer; the caller
hands in a destination `int8[]` and asks for up to N bytes per
call. EOF surfaces as a zero return — no `EndOfFileException`
unless the caller opted into "all-or-error" behavior via a
helper layer (Tier-2, not yet shipped).

## Surface

```cajeta
public class FileReader {
    public int32 read(int8[] buf, int32 max);   // bytes read; 0 == EOF
    public int64 position();                    // bytes consumed
    public void  close();                       // optional; ~FileReader closes
    public ~FileReader();                       // closes if still open
}
```

## Notes

- **`read(buf, max)`** — fills up to `max` bytes into `buf[0..max)`,
  returns the count. A return of 0 means EOF; the file remains
  open and re-read attempts return 0 forever. A short read
  (less than `max`) does NOT mean EOF — the caller loops on a
  non-zero return until it sees `0`. OS-level short reads are
  hidden by the internal buffer.
- **`position()`** — bytes consumed since open. Streaming model,
  not seekable; for seek/lock, open via `File.open(p, mode)`
  (Phase E).
- **Buffer ownership** — `buf` is the caller's array. The reader
  never retains a reference past the call.
- **Drop / destructor** — the destructor closes the underlying
  fd if `close()` wasn't called explicitly. Per
  `MemoryModel.md`, drop fires deterministically at scope exit.
- **No charset.** Bytes only. Decode externally via String.

## Idiomatic loop

```cajeta
FileReader r = File.openRead(p);
int8[] buf = heap int8[8192];
int32 n = r.read(buf, 8192);
while (n > 0) {
    consume(buf, n);
    n = r.read(buf, 8192);
}
// r drops on scope exit; fd auto-closes.
```

## See also

- [`FileWriter.md`](FileWriter.md) — symmetric write side.
- [`File.md`](File.md) — the `openRead` factory and the random-
  access alternative.
