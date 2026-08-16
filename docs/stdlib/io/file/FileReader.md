# FileReader

`cajeta.io.file.FileReader` — streaming byte reader, returned by
[`File.openRead(path)`](File.md). The caller drives the byte loop: call
`read(buf, max)` repeatedly until it returns `0` (EOF). The native read drains
OS-level short reads, so a return shorter than `max` (including `0`) means EOF
was reached on this call. There is no user-space buffer — bytes go straight
into the caller's array via the syscall. Call `close()` when done; there is no
auto-close-on-drop destructor yet.

```cajeta
FileReader r #= File.openRead("/var/log/app.log");
int8[] buf = heap int8[4096];
int32 n = r.read(buf, 4096);
while (n != 0) {
    // consume buf[0..n)
    n = r.read(buf, 4096);
}
r.close();
```

## Methods

| Signature | |
|---|---|
| `int32 read(int8[] buf, int32 max)` ⚑ | Read up to `max` bytes into `buf[0..max)`; returns the count filled, `0` at EOF |
| `int64 position()` | Bytes consumed since open (streaming model only — no seek) |
| `#String readString(int32 maxBytes)` ⚑ | Read up to `maxBytes` bytes and return them as a `String` (bytes taken as UTF-8, no transcoding) |
| `void close()` | Close the underlying fd; idempotent |

⚑ = `@EntryPoint`

## See also

- Tour: [FileIoDemo](../../../../samples/tour/src/main/cajeta/tour/io/FileIoDemo.cajeta)
- Source: [`runtime/src/cajeta/io/file/FileReader.cajeta`](../../../../runtime/src/cajeta/io/file/FileReader.cajeta)
- [File](File.md) — the factory (`openRead`); [FileWriter](FileWriter.md) — the writing twin
