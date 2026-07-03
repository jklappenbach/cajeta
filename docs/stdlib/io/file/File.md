# File

`cajeta.io.file.File` — filesystem access in two patterns under one class name:
static one-shots (`readAllBytes` / `writeAllBytes`, plus the `openRead` /
`openWrite` factories that return streaming [FileReader](FileReader.md) /
[FileWriter](FileWriter.md)), and an instance random-access handle returned by
`File.open(path, mode)` with seek, lock, truncate, sync, and positional
read/write. Open modes are selected with `OpenMode` (`READ`, `WRITE`, `APPEND`,
`READ_WRITE`, `CREATE_NEW`). An open handle exposes its OS descriptor as `fd`
(`-1` once closed) and its cached position as `pos`.

```cajeta
// Whole-file one-shot.
int8[] all = File.readAllBytes("/etc/hostname");

// Seekable random-access handle.
File f = File.open("/tmp/db.bin", OpenMode.READ_WRITE);
f.seek((int64) 4096);
int8[] page = heap int8[512];
int64 got = f.read(page, (int64) 0, (int64) 512);
f.close();
```

## Methods

| Signature | |
|---|---|
| `static #int8[] readAllBytes(String path)` ⚑ | Read the entire file at `path` into a freshly allocated `int8[]` |
| `static void writeAllBytes(String path, int8[] data, int32 len)` ⚑ | Write `data[0..len)` to `path` with atomic-rename semantics |
| `static #FileReader openRead(String path)` ⚑ | Open `path` for reading; return a streaming [FileReader](FileReader.md) |
| `static #FileWriter openWrite(String path, OpenMode mode)` ⚑ | Open `path` for writing in `mode`; return a streaming [FileWriter](FileWriter.md) |
| `static #File open(String path, OpenMode mode)` ⚑ | Open `path` as a seekable random-access `File` handle in `mode` |
| `static #File openExclusive(String path)` | Atomically create-and-open `path`, failing if it already exists (`O_CREAT \| O_EXCL`) — a lock-file / unique-create primitive |
| `int64 read(int8[] dst, int64 offset, int64 length)` | Read up to `length` bytes from the current position into `dst[offset..offset+length)`; returns the count filled, `0` at EOF |
| `int64 write(int8[] data, int64 offset, int64 length)` | Write `data[offset..offset+length)` at the current position; returns the count written |
| `int64 position()` | Current file position in bytes (cached — no syscall) |
| `void seek(int64 absolute)` | Move to absolute byte offset `absolute` |
| `void seekFromEnd(int64 offset)` | Seek relative to end-of-file (`0` lands at EOF) |
| `int64 size()` | Current file length in bytes (one `fstat()`) |
| `void truncate(int64 size)` | Resize the file to `size` bytes; extends with zeros, does not move `pos` |
| `void flush()` | No-op flush — there is no user-space buffer in v1 |
| `void sync()` | `fsync(2)` — durably flush every pending byte to disk before returning |
| `void lock()` | Blocking exclusive lock on this fd (POSIX `flock` `LOCK_EX`) |
| `boolean tryLock()` | Non-blocking exclusive lock attempt; `false` if another process holds it |
| `void unlock()` | Release a held lock; no-op if none is held |
| `void close()` | Close the underlying fd; idempotent |

⚑ = `@EntryPoint`

## See also

- Tour: [FileIoDemo](../../../../samples/tour/src/main/cajeta/tour/io/FileIoDemo.cajeta)
- Source: [`runtime/src/cajeta/io/file/File.cajeta`](../../../../runtime/src/cajeta/io/file/File.cajeta)
- [FileReader](FileReader.md) / [FileWriter](FileWriter.md) — the streaming halves; [Path](Path.md) — path queries and mutations
