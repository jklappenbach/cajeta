# Buffer\<T\>

`cajeta.io.Buffer` — a byte buffer with typed, reinterpreting access, backed by
an owned `int8[]`. `T` is the buffer's natural element type; the `load*` family
reads any width at an arbitrary byte offset (unaligned) — the SWAR /
serialization workhorse a plain `int8[]` can't express. v1 ships the byte/word
read core (`byteAt`, `loadU64`); strided element access, `reinterpret<U>`, and
the `store*` family land as scenarios demand them.

```cajeta
int8[] bytes = heap int8[16];
Buffer<int8> buf = heap Buffer<int8>(#bytes, (int64) 16);
int64 total = buf.byteCount();          // 16
int8 b0 = buf.byteAt((int64) 0);
int64 word = buf.loadU64((int64) 0);    // unaligned little-endian 64-bit load
```

## Methods

| Signature | |
|---|---|
| `Buffer(#int8[] data, int64 byteLength)` ⚑ | Wrap (take ownership of) `data`; `byteLength` is the valid byte count |
| `int64 byteCount()` | Total valid bytes |
| `int8 byteAt(int64 off)` | The byte at `off` |
| `int64 loadU64(int64 off)` | Unaligned little-endian 64-bit load at byte offset `off`; the caller keeps `off` in `[0, byteCount - 8]` (no bounds check) |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/Buffer.cajeta`](../../../runtime/src/cajeta/io/Buffer.cajeta)
- [KernelBuffer](../xpu/KernelBuffer.md) — the GPU-side device buffer counterpart
