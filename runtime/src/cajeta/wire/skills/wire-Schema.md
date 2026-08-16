---
id: wire-Schema
applies-to: [cajeta/wire/Schema]
title: Schema — opaque owned carrier of a format's raw schema bytes
description: Wrap raw schema-definition bytes (constructor takes #, definition() returns a borrow) to pass to a SchemaEncoder<T> for untagged formats like Avro.
---

# Schema

The opaque schema-bytes carrier in `cajeta.wire`. **Support/value type, not an access
point** — you construct it directly and hand it to a `SchemaEncoder<T>` (see
`cajeta/wire/SchemaEncoder`), which uses it to encode/decode **untagged** formats (Avro)
whose bytes are undecodable without their schema. Tagged formats (Protobuf, MessagePack)
use the schemaless `Encoder<T>` and never touch `Schema`.

In v1 it is a deliberately *opaque* wrapper: it carries the raw serialized definition
(for Avro, the JSON schema text an Object Container File embeds in its header) as owned
`int8[]` bytes. There is no field model yet.

## Construction & ownership

`heap`-construct it, transferring ownership of the definition bytes with `#`:

```cajeta
import cajeta.wire.Schema;

Schema s = heap Schema(#schemaBytes);   // # → Schema now owns the bytes
int8[] def = s.definition();            // borrow; still owned by s
```

- `Schema(#int8[] definition)` — **takes ownership** of `definition`. After the `#`
  transfer the caller must not free or reuse the passed array; its lifetime is the
  `Schema`'s.
- `int8[] definition()` — returns a **borrow** of the descriptor, owned by the `Schema`.
  Do not free it, and do not retain it past the `Schema`'s lifetime; copy if you need it
  to outlive the carrier.

Lifecycle is ordinary heap/drop — the bytes are freed when the `Schema` is dropped. There
is **no** `close()`/explicit-dispose obligation.

## Worked example — feed a SchemaEncoder<T>

```cajeta
import cajeta.wire.Schema;
import cajeta.wire.SchemaEncoder;

Schema schema = heap Schema(#schemaBytes);
SchemaEncoder<Order> codec = AvroEncoder<Order>();
#int8[] bytes = codec.encode(order, schema);   // caller owns the # result
Order  back  #= codec.decode(bytes, schema);    // decode is the inverse
```

`encode`/`decode` only **read** the `Schema` (it is passed by borrow, not `#`), so one
`Schema` can drive many calls and be shared across encode and decode.

## What Schema does NOT do

- **No parsing or field modeling** — no field/record introspection, resolution, aliases,
  or logical types in v1; it is purely the raw bytes. Structured modeling arrives with
  the first format that needs it (Avro, Phase 5).
- **No validation** — it does not check that the bytes are well-formed schema text; that
  is the `SchemaEncoder<T>` implementation's concern.
- **It is not the codec** — `Schema` holds no `T` and performs no encode/decode; that is
  `cajeta/wire/SchemaEncoder`.
