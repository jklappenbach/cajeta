# SchemaEncoder\<T\>

`cajeta.wire.SchemaEncoder` — schema-carrying wire codec for values of type
`T`. Identical in spirit to [`Encoder<T>`](Encoder.md), but every call also
takes a [Schema](Schema.md): it is the contract for untagged formats (e.g.
Avro) whose bytes are undecodable without the schema that produced them.
Tagged formats that carry their own structure (Protobuf, MessagePack) use the
thinner `Encoder<T>` instead. The two halves are inverses —
`decode(encode(value, s), s)` yields a value equal to the original — and both
directions transfer ownership of their heap result (`#`) to the caller.

```cajeta
import cajeta.wire.Schema;
import cajeta.wire.SchemaEncoder;

// Toy single-byte codec; a real untagged format (Avro) resolves `schema`.
public final class ByteCodec implements SchemaEncoder<int8> {
    public ByteCodec() { }

    public #int8[] encode(int8 value, Schema schema) {
        int8[] out = heap int8[1];
        out[0] = value;
        return #out;
    }

    public #int8 decode(int8[] bytes, Schema schema) {
        int8 v = bytes[0];
        return #v;
    }
}
```

## Methods

| Signature | |
|---|---|
| `#int8[] encode(T value, Schema schema)` ⚑ | Serialize `value` under `schema` into a caller-owned byte buffer |
| `#T decode(int8[] bytes, Schema schema)` ⚑ | Reconstruct a `T` from `bytes` under `schema`; inverse of `encode` |

⚑ = `@EntryPoint`

## See also

- [Schema](Schema.md) — the schema carrier every call takes
- [Encoder](Encoder.md) — the schema-free codec for tagged formats
- Source: [`runtime/src/cajeta/wire/SchemaEncoder.cajeta`](../../../runtime/src/cajeta/wire/SchemaEncoder.cajeta)
