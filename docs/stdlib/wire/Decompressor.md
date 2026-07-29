# Decompressor

`cajeta.wire.Decompressor` — the block-decompress stage as an explicit,
reusable component, the byte→byte inverse of [Compressor](Compressor.md).
Shared by every Class C container format (Parquet, ORC, Avro), which frame
their payloads as independently compressed blocks. The caller supplies
`expandedLen` (the known decompressed byte length, recorded in the container's
block header) so the output buffer is sized in one allocation.

```cajeta
import cajeta.wire.Decompressor;

// A pass-through stage: copies src into an expandedLen-sized buffer.
public final class IdentityDecompressor implements Decompressor {
    public IdentityDecompressor() { }

    public #int8[] decompress(int8[] src, int64 expandedLen) {
        int8[] out = heap int8[(int32) expandedLen];
        int32 i = 0;
        while (i < (int32) expandedLen) {
            out[i] = src[i];
            i = i + 1;
        }
        return #out;
    }
}
```

## Methods

| Signature | |
|---|---|
| `#int8[] decompress(int8[] src, int64 expandedLen)` ⚑ | Decompress `src` into a fresh, caller-owned buffer of exactly `expandedLen` bytes |

⚑ = `@EntryPoint`

## See also

- [Compressor](Compressor.md) — the byte→byte inverse stage
- [Encoder](Encoder.md) — the typed (`T`-carrying) wire codec
- Source: [`runtime/src/cajeta/wire/Decompressor.cajeta`](../../../runtime/src/cajeta/wire/Decompressor.cajeta)
