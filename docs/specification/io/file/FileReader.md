# `cajeta.io.file.FileReader` — streaming byte reader

Returned by `File.openRead(path)`. The caller hands in a
destination `int8[]` and asks for up to N bytes per call. There is
no user-space buffer — the destination array is passed straight to
the `read(2)` syscall; the runtime loops past partial OS reads so a
call fills the request fully (or stops at EOF). EOF surfaces as a
zero return — no `EndOfFileException` unless the caller opts into
"all-or-error" behavior via a helper layer (Tier-2, not yet
shipped).

## Surface

```cajeta
public class FileReader {
    public int32   fd;                          // OS fd, -1 once closed
    public int64   pos;                         // bytes consumed

    public int32   read(int8[] buf, int32 max); // bytes read; 0 == EOF
    public int64   position();                  // bytes consumed
    public #String readString(int32 maxBytes);  // up to maxBytes as UTF-8
    public void    close();                     // idempotent
}
```

## Notes

- **`read(buf, max)`** — fills up to `max` bytes into `buf[0..max)`,
  returns the count. A return of 0 means EOF; the file remains
  open and re-read attempts return 0 forever. Because the runtime
  loops past partial OS reads, a non-zero return *shorter* than
  `max` means EOF was reached mid-fill — the next call returns 0.
  Either way the `while (n > 0)` loop below terminates correctly.
- **`readString(maxBytes)`** — reads up to `maxBytes` bytes and
  wraps them in a freshly-allocated `#String` (owned, so closing
  the reader doesn't invalidate the text). Bytes are taken as
  UTF-8 with no charset decode at this layer; transcode via
  `Encoding` if the file isn't UTF-8. EOF mid-read returns
  whatever was read (possibly empty).
- **`position()`** — bytes consumed since open. Streaming model,
  not seekable; for seek/lock, open via `File.open(path, mode)`
  (Phase E).
- **Buffer ownership** — `buf` is the caller's array. The reader
  never retains a reference past the call.
- **Close** — `close()` is idempotent (sets `fd = -1`). The class
  does not yet declare a `~FileReader()` destructor, so auto-close
  on scope exit is *planned*, not wired — call `close()`
  explicitly today.
- **No charset.** Bytes only. Decode externally via `String`.

## Idiomatic loop

```cajeta
FileReader r #= File.openRead(p);
int8[] buf = heap int8[8192];
int32 n = r.read(buf, 8192);
while (n > 0) {
    consume(buf, n);
    n = r.read(buf, 8192);
}
r.close();   // explicit close (auto-close on drop is planned)
```

## See also

- [`FileWriter.md`](FileWriter.md) — symmetric write side.
- [`File.md`](File.md) — the `openRead` factory and the random-
  access alternative.
