# `cajeta.io.file.FileReader` — streaming byte reader

`File.openRead(path)` returns a `FileReader`. The caller hands in a
destination `int8[]` and asks for up to N bytes per call. There is
no user-space buffer. The destination array is passed straight to
the `read(2)` syscall, and the runtime loops past partial OS reads
so a call fills the request fully or stops at EOF. EOF surfaces as
a zero return. There is no `EndOfFileException` unless the caller
opts into "all-or-error" behavior via a helper layer, which is
Tier-2 and not yet shipped.

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

- **`read(buf, max)`** fills up to `max` bytes into `buf[0..max)`
  and returns the count. A return of 0 means EOF. The file remains
  open, and re-read attempts return 0 forever. Because the runtime
  loops past partial OS reads, a non-zero return shorter than
  `max` means EOF was reached mid-fill, and the next call returns
  0. Either way the `while (n > 0)` loop below terminates
  correctly.
- **`readString(maxBytes)`** reads up to `maxBytes` bytes and
  wraps them in a freshly-allocated `#String`. The string is
  owned, so closing the reader does not invalidate the text. Bytes
  are taken as UTF-8 with no charset decode at this layer, so
  transcode via `Encoding` if the file is not UTF-8. EOF mid-read
  returns whatever was read, which may be empty.
- **`position()`** is the count of bytes consumed since open. This
  is a streaming model and is not seekable. For seek and lock,
  open via `File.open(path, mode)` (Phase E).
- **Buffer ownership.** `buf` is the caller's array. The reader
  never retains a reference past the call.
- **Close.** `close()` is idempotent and sets `fd = -1`. The class
  does not yet declare a `~FileReader()` destructor, so auto-close
  on scope exit is planned and not wired. Call `close()`
  explicitly today.
- **No charset.** The reader handles bytes only. Decode externally
  via `String`.

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
