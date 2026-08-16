---
id: wire-overview
applies-to: [cajeta.wire]
title: cajeta.wire orientation — value codecs vs block (de)compressors
description: Pick a wire abstraction — Encoder vs SchemaEncoder for value codecs, Compressor/Decompressor for block compression — and learn the library-wide caller-owned-result and no-third-party rules.
---

# cajeta.wire — value codecs and block compression

The contracts for moving bytes across a process, file, or socket boundary. Two
unrelated jobs live here: **value codecs** turn a typed `T` into bytes and back
(`Encoder<T>`, `SchemaEncoder<T>`), and **block (de)compressors** turn bytes into
smaller bytes and back (`Compressor`, `Decompressor`). These are *interfaces only* —
`cajeta.wire` defines the shapes; concrete formats (JSON/Avro encoders, Snappy/Lz4/
Zstd/Gzip/Brotli compressors) implement them. The unit of currency everywhere is a
heap-owned `int8[]` byte buffer.

If your task is text interchange (Base64/CSV/JSON ⇄ bytes), that is **not here** — use
`cajeta.codec`. `cajeta.wire` is the layer beneath it: typed value serialization and
the block-compression stage that Class C container formats (Parquet, ORC, Avro) frame
their payloads with.

## Task → entry point

| Want to…                                                     | Start with                                  |
|--------------------------------------------------------------|---------------------------------------------|
| Serialize a `T` to bytes for a **self-describing/tagged** format (Protobuf, MessagePack, JSON) | implement/use `Encoder<T>` |
| Serialize a `T` for an **untagged** format that needs its schema to decode (Avro) | implement/use `SchemaEncoder<T>` |
| Carry the raw schema definition alongside untagged bytes     | `Schema` (wrap `#int8[]` descriptor)        |
| Compress a byte block                                        | `Compressor.compress(src)`                  |
| Decompress a byte block (you know the expanded length)       | `Decompressor.decompress(src, expandedLen)` |

**Negative rows (don't go hunting):**
- No concrete encoders/compressors live in `cajeta.wire` — it is interfaces +
  `Schema` only. `JsonEncoder`, `AvroEncoder`, `Snappy`, `Lz4`, `Zstd`, `Gzip`,
  `Brotli` are separate implementations against these contracts.
- No streaming/incremental codec — every method is one-shot over a whole buffer.
- `Decompressor` does **not** discover the output size; you must pass `expandedLen`
  (it is recorded in the container's block header).
- `Schema` v1 does **no** structured field modeling (resolution, aliases, logical
  types) — it is an opaque carrier of the raw descriptor bytes.

## Library-wide conventions (learn once, applies everywhere)

- **Every result is caller-owned (`#`).** All four interfaces return `#int8[]` (or
  `#T`): the fresh buffer/object is yours and drops at your scope. Parameters are
  **borrowed** — `encode`/`compress`/`decompress` take their `value`/`src`/`bytes`
  without a `#`, so the caller keeps and frees the input. The one ownership *transfer
  in* is `Schema(#int8[] definition)`: the constructor takes the descriptor bytes; do
  not free them after handing them over.
- **`Schema.definition()` returns a borrow.** It hands back the raw descriptor bytes
  owned by the `Schema` (no `#`) — don't free it, don't let it outlive the `Schema`;
  copy to keep.
- **Codecs are pure inverses.** `decode(encode(value)) == value`, and for schema
  codecs `decode(encode(value, s), s) == value`. Compressor/Decompressor are likewise
  byte→byte inverses.
- **All implementations are our own Cajeta — no third-party libraries.** The Snappy/
  Lz4/Zstd/Gzip(zlib)/Brotli compressors are written in Cajeta, not FFI bindings to C
  libs. Don't reach for a system zlib.
- **No lifecycle/close.** These are stateless contracts; instances drop with their
  scope. There is no `close()` to call.

## Canonical end-to-end example

```cajeta
import cajeta.wire.Encoder;

// A self-describing format round-trips with just Encoder<T>:
Encoder<Order> codec = JsonEncoder<Order>();

#int8[] bytes = codec.encode(order);   // T -> fresh, caller-owned bytes
Order  back  #= codec.decode(bytes);   // owned bytes -> caller-owned T
```

Untagged format — pair the codec with the `Schema` it was produced under:

```cajeta
import cajeta.wire.SchemaEncoder;
import cajeta.wire.Schema;

Schema schema = heap Schema(#schemaBytes);   // ctor takes ownership of the descriptor
SchemaEncoder<Order> codec = AvroEncoder<Order>();

#int8[] bytes = codec.encode(order, schema);
Order  back  #= codec.decode(bytes, schema); // same schema required to decode
```

Block compression (format-agnostic, no `T`):

```cajeta
import cajeta.wire.Compressor;
import cajeta.wire.Decompressor;

Compressor   c     = Snappy();
Decompressor d     = Snappy();
#int8[]      block = c.compress(plain);
#int8[]      plain2 = d.decompress(block, expandedLen);  // expandedLen from block header
```

## Disambiguation

- **`Encoder<T>` vs `SchemaEncoder<T>`** — pick by whether the wire bytes are
  self-describing. Tagged formats that carry their own structure (Protobuf,
  MessagePack, JSON) use `Encoder<T>`; untagged formats whose bytes are undecodable
  without their schema (Avro) use `SchemaEncoder<T>` and thread a `Schema` through
  both calls.
- **Codec vs Compressor** — `Encoder`/`SchemaEncoder` are typed (`T` ⇄ bytes);
  `Compressor`/`Decompressor` are byte ⇄ byte and carry no `T`. They are deliberately
  separate so a Class C container writer can compose one of each (encode the record,
  then compress the block).

## Setup

Pure Cajeta, no `@Native` bridge. Imports: `cajeta.wire.Encoder`,
`cajeta.wire.SchemaEncoder`, `cajeta.wire.Schema`, `cajeta.wire.Compressor`,
`cajeta.wire.Decompressor`. Concrete `Schema` field modeling arrives with the first
format that needs it (Avro, Phase 5).

## Go deeper

- Class: `cajeta/wire/Schema` (the opaque descriptor carrier).
- Sibling library: `cajeta.codec` for Base64/CSV/JSON text interchange.
