---
id: wire-compression
applies-to: [cajeta/wire/Compressor, cajeta/wire/Decompressor]
title: Block compress/decompress — the Compressor + Decompressor inverse pair
description: The byte-to-byte block codec for Class C containers — Compressor.compress and Decompressor.decompress are inverses; the caller supplies expandedLen to size the output.
---

# Block compression (Compressor + Decompressor)

`Compressor` and `Decompressor` are the two halves of one byte-to-byte block
codec. `Compressor.compress(plain)` shrinks a byte buffer into a compressed
block; `Decompressor.decompress(block, expandedLen)` restores it. They are exact
inverses: `decompress(compress(plain), plain.count())` yields bytes equal to
`plain`. Use this pair when a Class C container format (Parquet, ORC, Avro)
frames its payload as independently compressed blocks.

Both are **format-agnostic interfaces carrying no `T`** — they move raw bytes,
not typed values. If you need value⇆bytes for a typed `T`, that is a different
concern: use `Encoder<T>` (`cajeta/wire/Encoder`) instead. The two are
deliberately separate; an `Encoder` may compress its output by handing the bytes
to a `Compressor`, but the interfaces never mix.

## Members and roles

- **`Compressor`** — one method, `#int8[] compress(int8[] src)`. The
  block-compress stage.
- **`Decompressor`** — one method, `#int8[] decompress(int8[] src, int64 expandedLen)`.
  The block-decompress stage. `expandedLen` is the decompressed byte length.

Concrete implementations are `Snappy`, `Lz4`, `Zstd`, `Gzip`/`zlib`, and
`Brotli` — **all our own Cajeta, no third-party libraries**. Each implementation
type typically provides both directions, so you instantiate one object and use it
as either interface.

## Collaboration and data flow

The two halves do **not** share any session or state object — they cooperate
only through the bytes carried between them, plus one out-of-band number:
`expandedLen`. The codec block is **not self-describing about its output size**;
`Decompressor` does not discover the decompressed length from the block. The
caller must record the original length at compress time and supply it at
decompress time. In a Class C container that length lives in the **block header**
the format writes alongside each block, and is read back from that header — see
the container format's reader/writer, not this codec.

So the end-to-end choreography across the boundary is:

1. Writer side: `block = compressor.compress(plain)`; the container records
   `plain.count()` as the block's `expandedLen` in its header.
2. Reader side: read the block bytes and its `expandedLen` from the header, then
   `plain = decompressor.decompress(block, expandedLen)`.

The compressor and decompressor on the two sides **must be the same algorithm**
(a Snappy block must be read by a Snappy `Decompressor`); the pair has no
algorithm tag of its own.

## Ownership and lifecycle (read before crossing the boundary)

- **Inputs are borrowed.** Neither `src` parameter is marked `#` — the codec
  reads the input buffer and neither frees nor retains it. The caller keeps owning
  `src` and frees it on its own drop chain.
- **Results are owned (`#`).** Both methods return a **fresh, caller-owned**
  `#int8[]`; ownership transfers to you and your drop chain reclaims it. Results
  never alias the input.
- **`decompress` returns exactly `expandedLen` bytes**, allocated in one shot —
  that is the whole point of taking the length up front.
- These are **stateless byte transforms**: an implementation instance holds no
  per-call state, so it is reusable across many blocks. (Consult the concrete
  implementation skill for any thread/fiber-safety guarantee.)

## Worked example

```cajeta
import cajeta.wire.Compressor;
import cajeta.wire.Decompressor;
import cajeta.wire.Snappy;

Snappy codec = Snappy();

// write side — keep the original length for the block header
int64 expandedLen = plain.count();
#int8[] block = ((Compressor) codec).compress(plain);

// read side — header gives back expandedLen
#int8[] restored = ((Decompressor) codec).decompress(block, expandedLen);
// restored now equals plain, byte for byte
```

(You normally hold the value as the interface type — `Compressor c = Snappy();`
on the writer and `Decompressor d = Snappy();` on the reader; the casts above
just show one object satisfying both.)

## What this pair does NOT do

- **No size self-description.** `decompress` will not work from the block alone —
  you must supply `expandedLen`. Lose it and the block is unrecoverable through
  this interface. Persist it (the container's block header) when you compress.
- **No block framing, headers, or checksums.** This is the raw byte transform
  only; per-block length/offset/CRC framing belongs to the Class C container
  format, not here.
- **No streaming / no chunking.** It is one-shot, whole-buffer-in-memory per
  block; both the input and the full result live in memory at once. Size your
  blocks at the container layer.
- **Carries no `T` and does no value (de)serialization** — that is `Encoder<T>`.
- **Does not consume or mutate `src`**, and does not pick the algorithm for you;
  matching compressor↔decompressor algorithm is the caller's responsibility.
