# Schema

`cajeta.wire.Schema` — the schema a [`SchemaEncoder<T>`](SchemaEncoder.md)
decodes against. v1 is a deliberately opaque carrier of the schema's raw
serialized definition — for Avro that is the JSON schema text an Object
Container File embeds in its header. Until a format needs structured field
modeling, a `Schema` is just the owned bytes that describe a record.

```cajeta
int8[] def = heap int8[4];
Schema s = heap Schema(#def);
int8[] raw = s.definition();   // borrow the raw descriptor
```

## Methods

| Signature | |
|---|---|
| `Schema(#int8[] definition)` ⚑ | Wrap (take ownership of) the raw schema `definition` bytes |
| `int8[] definition()` | The raw schema descriptor bytes (borrowed; owned by this `Schema`) |

⚑ = `@EntryPoint`

## See also

- [SchemaEncoder](SchemaEncoder.md) — the codec that consumes a `Schema`
- Source: [`runtime/src/cajeta/wire/Schema.cajeta`](../../../runtime/src/cajeta/wire/Schema.cajeta)
