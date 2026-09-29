# Decompressor

`cajeta.wire.Decompressor` is the decompress stage of a byte-to-byte codec, the
inverse of [Compressor](Compressor.md). A block container knows each block's
decompressed length and passes it as `expandedLen`. A transport that does not,
such as HTTP content coding, passes a cap instead, and the codec raises
[DecompressionLimitException](DecompressionLimitException.md) rather than
produce more.

```cajeta
import cajeta.wire.Decompressor;
import cajeta.wire.DecompressionLimitException;

// A pass-through codec showing the contract's shape.
public final class CopyDecompressor implements Decompressor {
    public CopyDecompressor() { }

    static #int8[] take(int8[] src, int64 len) {
        int8[] out = heap int8[len];
        int64 i = 0;
        while (i < len) {
            out[i] = src[i];
            i = i + 1;
        }
        return #out;
    }

    public #int8[] decompress(int8[] src, int64 expandedLen) {
        return CopyDecompressor.take(src, expandedLen);
    }

    public #int8[] decompress(int8[] src, int64 len, int64 maxOut) {
        if (len > maxOut) {
            throw heap DecompressionLimitException("output over the cap", maxOut);
        }
        return CopyDecompressor.take(src, len);
    }
}
```

## Methods

| Signature | |
|---|---|
| `#int8[] decompress(int8[] src, int64 expandedLen)` ⚑ | Decompress `src` into a fresh, caller-owned buffer of exactly `expandedLen` bytes |
| `#int8[] decompress(int8[] src, int64 len, int64 maxOut)` ⚑ | Decompress `src[0 .. len)`, whose output size is unknown. Raises `DecompressionLimitException` rather than exceed `maxOut` bytes |

⚑ = `@EntryPoint`

## See also

- [Compressor](Compressor.md), the inverse stage
- [DecompressionLimitException](DecompressionLimitException.md)
- Source: [`runtime/src/cajeta/wire/Decompressor.cajeta`](../../../runtime/src/cajeta/wire/Decompressor.cajeta)
