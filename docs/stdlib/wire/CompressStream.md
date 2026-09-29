# CompressStream

`cajeta.wire.CompressStream` is an open compression, made by
[`Compressor.stream(level)`](Compressor.md). Bytes written are compressed as
they arrive. `flush` hands out everything needed to decode what was written so
far, which is what a streamed HTTP body sends as each piece. `finish` ends the
stream with the format's trailer.

```cajeta
import cajeta.wire.CompressStream;

// Holds what is written and hands it back unchanged.
public final class CopyStream implements CompressStream {
    int8[] held;
    int64 n;

    public CopyStream() {
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
        int8[] out = heap int8[this.n];
        int64 i = 0;
        while (i < this.n) {
            out[i] = this.held[i];
            i = i + 1;
        }
        this.n = 0;
        return #out;
    }

    public #int8[] finish() {
        return this.flush();
    }
}
```

## Methods

| Signature | |
|---|---|
| `void write(int8[] src, int64 len)` ⚑ | Compress `src[0 .. len)`. Output is held until `flush` or `finish` |
| `#int8[] flush()` ⚑ | The compressed bytes since the last call, ending where everything written so far decodes. May be empty |
| `#int8[] finish()` ⚑ | The remaining bytes and the trailer. The stream takes no more writes |

⚑ = `@EntryPoint`

## See also

- [Compressor](Compressor.md)
- Source: [`runtime/src/cajeta/wire/CompressStream.cajeta`](../../../runtime/src/cajeta/wire/CompressStream.cajeta)
