# Encoder\<T\>

`cajeta.wire.Encoder` — bidirectional wire codec for values of type `T`: turns
a `T` into a heap-owned byte buffer and reconstructs a `T` from one. Implement
it once per serialization format and hand instances around wherever a value
crosses a process, file, or socket boundary. The two halves are inverses —
`decode(encode(value))` yields a value equal to the original — and both
directions transfer ownership of their heap result (`#`) to the caller.

```cajeta
import cajeta.wire.Encoder;

// A concrete little-endian Encoder<int32>.
public final class Int32Codec implements Encoder<int32> {
    public Int32Codec() { }

    public #int8[] encode(int32 value) {
        int8[] b = heap int8[4];
        b[0] = (int8) (value & 0xFF);
        b[1] = (int8) ((value >> 8) & 0xFF);
        b[2] = (int8) ((value >> 16) & 0xFF);
        b[3] = (int8) ((value >> 24) & 0xFF);
        return #b;
    }

    public #int32 decode(int8[] bytes) {
        int32 b0 = ((int32) bytes[0]) & 0xFF;
        int32 b1 = ((int32) bytes[1]) & 0xFF;
        int32 b2 = ((int32) bytes[2]) & 0xFF;
        int32 b3 = ((int32) bytes[3]) & 0xFF;
        int32 r = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
        return #r;
    }
}
```

## Methods

| Signature | |
|---|---|
| `#int8[] encode(T value)` ⚑ | Serialize `value` into a freshly allocated, caller-owned byte buffer |
| `#T decode(int8[] bytes)` ⚑ | Reconstruct a `T` from its encoded `bytes`; inverse of `encode` |

⚑ = `@EntryPoint`

## See also

- Tour: [Int32Encoder](../../../samples/tour/src/main/cajeta/tour/collection/Int32Encoder.cajeta),
  [LtmBPlusTreeDemo](../../../samples/tour/src/main/cajeta/tour/collection/LtmBPlusTreeDemo.cajeta)
- [SchemaEncoder](SchemaEncoder.md) — the schema-carrying variant for untagged formats
- Source: [`runtime/src/cajeta/wire/Encoder.cajeta`](../../../runtime/src/cajeta/wire/Encoder.cajeta)
