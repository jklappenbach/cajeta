# DecompressionLimitException

`cajeta.wire.DecompressionLimitException` is raised by
[`Decompressor.decompress(src, len, maxOut)`](Decompressor.md) when the output
would exceed the caller's cap. It is the guard against a small input that
expands without bound. It extends `RecoverableException`.

| Member | |
|---|---|
| `int64 limit` | The cap the caller set, in bytes |
| `DecompressionLimitException(#String message, int64 limit)` | A limit failure for a cap of `limit` bytes |

## See also

- [Decompressor](Decompressor.md)
- Source: [`runtime/src/cajeta/wire/DecompressionLimitException.cajeta`](../../../runtime/src/cajeta/wire/DecompressionLimitException.cajeta)
