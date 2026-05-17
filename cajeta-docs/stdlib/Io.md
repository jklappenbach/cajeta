# `cajeta.io` — Byte substrate + stream abstractions

Umbrella for everything that crosses the program / outside-world
boundary. Direct members are the shared abstractions; concrete I/O
kinds (file, network, subprocess) live in nested subpackages so a
program that only needs files doesn't drag in a TLS stack.

Status: **designed, not implemented**. Tracked in Features.md.

## `Buffer` + `BufferChain` — the byte substrate

```cajeta
public class Buffer {
    public Buffer(int64 capacity);
    public int64 capacity();
    public byte[] data();
    public Buffer next();
    public void linkNext(Buffer next);
}

public class BufferChain {
    public void append(Buffer b);
    public Buffer head();
    public int64 totalSize();
}
```

`Buffer` wraps a single `byte[MAX_SIZE]`. `BufferChain` is the linked
list of buffers used by the harness (multiple-inherits `Stream<Buffer>`
for chain traversal).

## `InputStream` / `OutputStream` / `Reader` / `Writer`

Java-shaped abstractions, byte buffers underneath. Byte-level
(`InputStream` / `OutputStream`) wrapped to code-point-level
(`Reader` / `Writer`) with an `Encoding`. The same interfaces back
files, sockets, subprocess stdio, and in-memory streams — every
concrete I/O type implements them so generic code (compress /
decompress, parse / serialize) works over any source.

## Example shape

```cajeta
InputStream src = someFile.inputStream();
OutputStream dst = otherFile.outputStream();

Buffer buf = heap Buffer(8192);
while (src.read(buf) > 0) {
    dst.write(buf);
}
```

## Open items

All of `cajeta.io` (Buffer, BufferChain, InputStream, OutputStream,
Reader, Writer) is unimplemented. Tracked in Features.md. Land along
with the fiber reactor and the first concrete I/O subpackage that
needs them.

## See also

- `cajeta.io.file` — file handle + path manipulation (see IoFile.md)
- `cajeta.io.net` — TCP / UDP / TLS / HTTP / WebSocket (see IoNet.md)
- `cajeta.process` — subprocess management (see Process.md)
