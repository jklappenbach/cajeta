# Compressor

`cajeta.wire.Compressor` — the block-compress stage, the byte→byte inverse of
[Decompressor](Decompressor.md), paired with it on every Class C container
writer (Parquet, ORC, Avro). Format-agnostic and carries no `T`, so it is
deliberately separate from [`Encoder<T>`](Encoder.md).

```cajeta
import cajeta.wire.Compressor;

// A pass-through stage showing the contract's shape.
public final class IdentityCompressor implements Compressor {
    public IdentityCompressor() { }

    public #int8[] compress(int8[] src) {
        int32 n = (int32) src.count();
        int8[] out = heap int8[n];
        int32 i = 0;
        while (i < n) {
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
| `#int8[] compress(int8[] src)` ⚑ | Compress `src` into a fresh, caller-owned block buffer |

⚑ = `@EntryPoint`

## See also

- [Decompressor](Decompressor.md) — the byte→byte inverse stage
- [Encoder](Encoder.md) — the typed (`T`-carrying) wire codec
- Source: [`runtime/src/cajeta/wire/Compressor.cajeta`](../../../runtime/src/cajeta/wire/Compressor.cajeta)
