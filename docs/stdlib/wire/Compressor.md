# Compressor

`cajeta.wire.Compressor` is the compress stage of a byte-to-byte codec, the
inverse of [Decompressor](Decompressor.md). It carries no `T`, so it stays
separate from [`Encoder<T>`](Encoder.md). Block containers (Parquet, ORC, Avro)
call `compress(src)`. HTTP content coding picks a level with
`compress(src, len, level)` and streams a body through `stream(level)`.

```cajeta
import cajeta.wire.Compressor;
import cajeta.wire.CompressStream;

// The stream a pass-through codec hands out: it returns what was written.
final class PassStream implements CompressStream {
    int8[] held;
    int64 n;

    PassStream() {
        this.held = heap int8[4096];
        this.n = 0;
    }

    public void write(int8[] src, int64 len) {
        int64 i = 0;
        while (i < len) {
            this.held[this.n + i] = src[i];
            i = i + 1;
        }
        this.n = this.n + len;
    }

    public #int8[] flush() {
        int8[] out #= CopyCompressor.take(this.held, this.n);
        this.n = 0;
        return #out;
    }

    public #int8[] finish() {
        return this.flush();
    }
}

// A pass-through codec showing the contract's shape.
public final class CopyCompressor implements Compressor {
    public CopyCompressor() { }

    static #int8[] take(int8[] src, int64 len) {
        int8[] out = heap int8[len];
        int64 i = 0;
        while (i < len) {
            out[i] = src[i];
            i = i + 1;
        }
        return #out;
    }

    public #int8[] compress(int8[] src) {
        return CopyCompressor.take(src, src.count());
    }

    public #int8[] compress(int8[] src, int64 len, int32 level) {
        return CopyCompressor.take(src, len);
    }

    public #CompressStream stream(int32 level) {
        return heap PassStream();
    }
}
```

## Methods

| Signature | |
|---|---|
| `#int8[] compress(int8[] src)` ⚑ | Compress all of `src` into a fresh, caller-owned block |
| `#int8[] compress(int8[] src, int64 len, int32 level)` ⚑ | Compress `src[0 .. len)` at `level` (1 fastest to 9 smallest for DEFLATE codecs). A codec without levels ignores it |
| `#CompressStream stream(int32 level)` ⚑ | A [CompressStream](CompressStream.md) that compresses what is written to it |

⚑ = `@EntryPoint`

## See also

- [Decompressor](Decompressor.md), the inverse stage
- [CompressStream](CompressStream.md), the streaming form
- [Encoder](Encoder.md), the typed (`T`-carrying) wire codec
- Source: [`runtime/src/cajeta/wire/Compressor.cajeta`](../../../runtime/src/cajeta/wire/Compressor.cajeta)
