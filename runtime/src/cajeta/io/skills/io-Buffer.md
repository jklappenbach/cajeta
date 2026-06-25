---
id: io-Buffer
applies-to: [cajeta/io/Buffer]
title: Buffer<T> — owned byte buffer with unaligned reinterpreting loads
description: Read-only byte buffer; loadU64 does unaligned little-endian 64-bit reads with NO bounds check (SWAR primitive).
---

# Buffer<T>

A byte buffer with **typed, reinterpreting reads at arbitrary byte offsets** —
the unaligned `load*` access a plain `int8[]` can't express (SWAR scanners,
serialization). Backed by an **owned `int8[]`**. This is a leaf value type in
`cajeta.io` (not the `cajeta.io.net` `ByteBuffer`/`RingBuffer`/`BufferPool`
cursor-and-pool layer — those are a different package).

**Access point:** yes — you construct it directly.

## Construction & ownership

```cajeta
import cajeta.io.Buffer;

int8[] bytes = heap int8[n];
// ... fill bytes ...
Buffer<int8> buf = heap Buffer<int8>(#bytes, n);   // # transfers ownership
int64 word = buf.loadU64(off);                      // unaligned LE 64-bit load
```

- `Buffer(#int8[] data, int64 byteLength)` — **takes ownership** of `data` (the
  `#` transfer). After the call, do not keep using your reference to `bytes`; the
  `Buffer` owns and will free the backing array when it drops. `byteLength` is the
  valid byte count, which need not equal `data.length`.
- `T` is the buffer's natural element type but v1 only exercises the byte/word read
  core; the type parameter doesn't gate the `load*`/`byteAt` API.

## Methods that matter

- `int64 byteCount()` — total valid bytes (the `byteLength` you passed in).
- `int8 byteAt(int64 off)` — the byte at `off`.
- `int64 loadU64(int64 off)` — **unaligned little-endian 64-bit load** at byte
  offset `off`. Lowers to a single `load i64, align 1`.

## Sharp edge — loadU64 has NO bounds check

`loadU64` does **not** validate `off`. The caller must keep
`off` in `[0, byteCount() - 8]`. Reading past the end is undefined (out-of-bounds
memory read), by design — this is the hot primitive for SWAR scanners where the
caller already owns the loop bound. Guard it yourself:

```cajeta
if (off <= buf.byteCount() - 8) {
    int64 w = buf.loadU64(off);
}
```

## What v1 does NOT do (don't hunt for these)

- **Read-only.** There is no `store*` family, no `at`/`set` strided element
  access, and no `reinterpret<U>`. To build content, fill the `int8[]` *before*
  handing it to the constructor.
- **No explicit endianness variants.** `loadU64` is little-endian only.
- **Not the networking buffer.** Cursors, pooling, ring/backpressure, and
  `read`/`write` live in `cajeta.io.net` (`ByteBuffer`, `RingBuffer`,
  `BufferPool`) — a separate package. The GPU device buffer is
  `cajeta.xpu.KernelBuffer<T>`.
