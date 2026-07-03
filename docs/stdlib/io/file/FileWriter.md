# FileWriter

`cajeta.io.file.FileWriter` — streaming byte writer, returned by
[`File.openWrite(path, mode)`](File.md). The caller hands in `(data, len)` per
call; the native write loops past OS-level short writes so every byte lands,
but there is no user-space buffer — bytes go straight to the syscall. Write
`String` payloads with `writeString`, raw bytes with `write`. Call `close()`
when done.

```cajeta
FileWriter w = File.openWrite("/tmp/out.txt", OpenMode.APPEND);
w.writeString("first line\n");
int8[] bytes = heap int8[2];
w.write(bytes, 2);
w.flush();
w.close();
```

## Methods

| Signature | |
|---|---|
| `void write(int8[] data, int32 len)` ⚑ | Write `data[0..len)` to the file; tolerates partial OS writes internally |
| `void writeString(String s)` ⚑ | Write a `String`'s UTF-8 bytes to the file |
| `void flush()` | Currently a no-op (writes are unbuffered); does not `fsync` — durability is [`File.sync()`](File.md) |
| `void close()` | Flush + close the underlying fd; idempotent |

⚑ = `@EntryPoint`

## See also

- Tour: [FileIoDemo](../../../../samples/tour/src/main/cajeta/tour/io/FileIoDemo.cajeta)
- Source: [`runtime/src/cajeta/io/file/FileWriter.cajeta`](../../../../runtime/src/cajeta/io/file/FileWriter.cajeta)
- [File](File.md) — the factory (`openWrite`); [FileReader](FileReader.md) — the reading twin
