# DefaultHasher

`cajeta.hash.DefaultHasher` — the hasher the compiler-synthesized
`Object.hash()` uses under the hood: process-seeded XXH3-64. `@AutoHash`
field-folding routes through this same shape, and `String.hash()` does too.
Internally it is a [XXHash3](XXHash3.md) constructed with the process seed,
exposed as a distinct class so the compiler couples to `DefaultHasher` rather
than to a specific algorithm. Construct one to compose an object's structural
hash with extra context, or with an explicit seed to reproduce a digest
computed in another process (get the seed from `Hash.processSeed()`).

```cajeta
DefaultHasher hasher = heap DefaultHasher();
Object order = heap Object();
hasher.writeObject(order);        // structural fold of the object
hasher.writeString("tenant-42");  // extra context
int64 digest = hasher.finish();
```

## Methods

| Signature | |
|---|---|
| `DefaultHasher()` ⚑ | Construct with the per-process seed — digests match the compiler-synthesized `Object.hash()` |
| `DefaultHasher(int64 seedArg)` ⚑ | Construct with an explicit seed — pass a seed captured from `Hash` in another process to reproduce its digest |
| `void writeBoolean(boolean v)` | Fold `v` into the backing `XXHash3` as one byte: `1` for true, `0` for false |
| `void writeInt8(int8 v)` | Fold the raw byte of `v` |
| `void writeInt16(int16 v)` | Fold the 2 raw bytes of `v` |
| `void writeInt32(int32 v)` | Fold the 4 raw bytes of `v` |
| `void writeInt64(int64 v)` | Fold the 8 raw bytes of `v` |
| `void writeUInt8(uint8 v)` | Fold the raw byte of `v` |
| `void writeUInt16(uint16 v)` | Fold the 2 raw bytes of `v` |
| `void writeUInt32(uint32 v)` | Fold the 4 raw bytes of `v` |
| `void writeUInt64(uint64 v)` | Fold the 8 raw bytes of `v` |
| `void writeFloat32(float32 v)` | Fold the 4 raw bytes of `v` |
| `void writeFloat64(float64 v)` | Fold the 8 raw bytes of `v` |
| `void writeBytes(int8[] data)` | Fold every byte of `data` |
| `void writeBytesRange(int8[] data, int64 offset, int64 length)` | Fold `length` bytes of `data` |
| `void writeString(String s)` | Fold the UTF-8 content of the `String` (same shape as `String.hash()`) |
| `void writeObject(Object obj)` | Fold the structural hash of the `Object` — the `@AutoHash` field-folding shape |
| `int64 finish()` | Close the stream and return the 64-bit digest of everything written so far |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/hash/DefaultHasher.cajeta`](../../../runtime/src/cajeta/hash/DefaultHasher.cajeta)
- [XXHash3](XXHash3.md) — the backing algorithm, [Hash](Hash.md) — the per-process seed
