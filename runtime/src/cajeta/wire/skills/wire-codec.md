---
id: wire-codec
applies-to: [cajeta/wire/Encoder, cajeta/wire/SchemaEncoder, cajeta/wire/Schema]
title: Value codecs — Encoder (tagged) vs SchemaEncoder + Schema (untagged)
description: Pick Encoder for self-describing formats, SchemaEncoder+Schema for untagged ones; encode/decode are owned-bytes inverses.
---

# Value codecs: `Encoder<T>`, `SchemaEncoder<T>`, `Schema`

The value tier of `cajeta.wire` — turn a `T` into bytes and back. **Pick by whether
the bytes are self-describing:**

- **`Encoder<T>`** — *tagged* formats whose bytes carry their own structure (Protobuf,
  MessagePack, JSON, CBOR). `encode`/`decode` need nothing but the value/bytes.
- **`SchemaEncoder<T>` + `Schema`** — *untagged* formats whose bytes are undecodable
  without the schema that produced them (Avro). Every call also takes a `Schema`.

If you are not (de)serializing a typed value `T` — you just need block byte→byte
compression for a container format — this is the **wrong** component: use
`Compressor`/`Decompressor` (separate, format-agnostic, no `T`) instead.

## Members and roles

| Type | Role | Access point? |
|------|------|---------------|
| `Encoder<T>` | interface; codec for tagged formats | yes — implement / hold an instance |
| `SchemaEncoder<T>` | interface; codec for untagged formats, threads a `Schema` | yes |
| `Schema` | `final class`; opaque carrier of a format's raw serialized schema definition | support/value — pass to a `SchemaEncoder` |

## Contract (both interfaces)

The two halves are **inverses**:

- `Encoder`:        `decode(encode(value))` equals the original.
- `SchemaEncoder`:  `decode(encode(value, s), s)` equals the original — the *same*
  schema must be threaded through both directions.

**Ownership at every boundary** (all results are heap-owned, marked `#`, transferred to
the caller):

- `#int8[] encode(T value)` / `#int8[] encode(T value, Schema schema)` → caller owns the
  returned buffer.
- `#T decode(int8[] bytes)` / `#T decode(int8[] bytes, Schema schema)` → caller owns the
  returned `T`. The input `bytes` are **borrowed** (not `#`): the codec does not take or
  free them.

## `Schema` ownership (the boundary that bites)

```cajeta
public Schema(#int8[] definition)   // TAKES ownership of the bytes
public int8[] definition()          // BORROWS them back — owned by the Schema
```

The constructor consumes its `#int8[]` (pass with `#`); `definition()` returns a
**borrowed** view, valid only while the `Schema` lives — copy it if you need it longer.
v1 is a deliberately opaque carrier (for Avro, the JSON schema text); it does **not**
model fields, resolution, aliases, or logical types yet.

## Worked example — untagged round-trip (from CodecTierInterfacesTests)

```cajeta
import cajeta.wire.SchemaEncoder;
import cajeta.wire.Schema;

// codec is some AvroEncoder<Order> / your SchemaEncoder<Order> impl
int8[] def = avroSchemaBytes();
Schema s = heap Schema(#def);            // # transfers def into the Schema

#int8[] bytes = codec.encode(order, s);  // T  -> owned bytes  (same schema...)
Order  back  #= codec.decode(bytes, s);  // bytes -> owned T    (...both ways)
```

Tagged is identical minus the `Schema` argument:

```cajeta
import cajeta.wire.Encoder;

#int8[] bytes = codec.encode(order);     // Encoder<Order>, no schema
Order  back  #= codec.decode(bytes);
```

To author a codec, `implements Encoder<T>` (or `SchemaEncoder<T>`) and return `heap`
buffers/instances from `encode`/`decode`; obtain an instance with `heap MyCodec()`.

## Choosing

Self-describing bytes → `Encoder<T>`. Bytes that require their producing schema to read →
`SchemaEncoder<T>` + `Schema`. Don't reach for `SchemaEncoder` on a tagged format just to
pass metadata — keep tagged codecs on the thinner `Encoder<T>`.
