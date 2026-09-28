---
id: wire-compression
applies-to: [cajeta/wire/Compressor, cajeta/wire/Decompressor, cajeta/wire/CompressStream, cajeta/wire/DecompressionLimitException]
title: Compressor, Decompressor and CompressStream — byte-to-byte codecs, block, capped and streamed
description: The byte-to-byte codec pair. Block containers pass the decompressed length; transports that cannot pass a cap and get DecompressionLimitException. CompressStream compresses a body as it is produced.
---

# Compressor, Decompressor and CompressStream

`Compressor` and `Decompressor` are the two halves of one byte-to-byte codec.
They carry no `T`. For value to bytes, use `Encoder<T>` instead. An `Encoder`
may hand its output to a `Compressor`, but the interfaces never mix.

No stdlib type implements them yet. A codec library or a transport supplies
the implementations, such as cajeta-http's content codings (gzip, deflate,
identity). One class usually implements both halves.

## Members

`Compressor`:

- `#int8[] compress(int8[] src)` compresses all of `src` into a fresh block.
- `#int8[] compress(int8[] src, int64 len, int32 level)` compresses
  `src[0 .. len)` at `level`, 1 fastest to 9 smallest for DEFLATE codecs. A
  codec without levels ignores the level.
- `#CompressStream stream(int32 level)` opens a stream.

`Decompressor`:

- `#int8[] decompress(int8[] src, int64 expandedLen)` restores a block whose
  decompressed length the caller recorded, such as a Parquet or ORC block
  header. The result is exactly `expandedLen` bytes, sized in one allocation.
- `#int8[] decompress(int8[] src, int64 len, int64 maxOut)` restores
  `src[0 .. len)` when the decompressed length is unknown, as an HTTP body's
  is. It raises `DecompressionLimitException` (field `limit`) rather than
  produce more than `maxOut` bytes. That cap is the guard against a small
  input that expands without bound, so every caller must state one.

`CompressStream`:

- `void write(int8[] src, int64 len)` compresses more input.
- `#int8[] flush()` returns the output since the last call, ending on a
  boundary where everything written so far decodes. A streamed response sends
  each flush as one piece.
- `#int8[] finish()` returns the rest and the format's trailer. No writes
  after it.

## Ownership

- Inputs are borrowed. No method frees or keeps `src`.
- Every result is a fresh, caller-owned `#int8[]` that never aliases the input.
  Bind it with `#=`.
- `stream(level)` returns an owned stream. Its state lives until you drop it.
- A `Compressor` or `Decompressor` instance holds no per-call state, so one
  instance serves many calls.

## Example

```cajeta
import cajeta.wire.Compressor;
import cajeta.wire.Decompressor;
import cajeta.wire.CompressStream;
import cajeta.wire.DecompressionLimitException;

// codec implements both Compressor and Decompressor.
Compressor c = codec;
Decompressor d = codec;

int8[] block #= c.compress(plain, plain.count(), 6);
try {
    int8[] back #= d.decompress(block, block.count(), (int64) 1048576);
} catch (DecompressionLimitException e) {
    // more than 1 MiB would have come out
}

CompressStream s #= c.stream(1);
s.write(piece, n);
int8[] out #= s.flush();
int8[] tail #= s.finish();
```

## What this does not do

- It adds no framing, length headers or checksums beyond the codec format's
  own. Container framing belongs to the container.
- It does not pick the algorithm. Matching compressor and decompressor is the
  caller's job, and the pair carries no algorithm tag.
- `CompressStream` has no decompressing twin yet. Decompression is one call
  over the whole input.
