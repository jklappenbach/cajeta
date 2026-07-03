# XXHash3

`cajeta.hash.XXHash3` — XXH3-64, the fast general-purpose 64-bit
non-cryptographic hash: the default backing algorithm for
compiler-synthesized `Object.hash()`, `String.hash()`, and `@AutoHash`.
Multi-GB/s throughput, with a small-input path (the typical field-hashing
case) of a handful of cycles. Pick over [SipHash](SipHash.md) when the input
is not attacker-controlled (~10x faster); the 128-bit `hash128` family is
the modern replacement for MD5-style fingerprints.

```cajeta
int64 h = XXHash3.hashString("hello");   // process-seeded

XXHash3 x = heap XXHash3();
x.writeInt32(7);
x.writeString("title");
int64 digest = x.finish();
```

## Methods

| Signature | |
|---|---|
| **One-shot statics (64-bit)** | |
| `static int64 hash(int8[] data, int64 len)` ⚑ | Hash of `data[0..len)` under the per-process seed |
| `static int64 hashSeeded(int8[] data, int64 len, int64 seedArg)` | Hash under an explicit `seedArg`, for reproducible digests |
| `static int64 hashString(String s)` ⚑ | Hash of a `String`'s UTF-8 bytes under the per-process seed |
| `static int64 hashStringSeeded(String s, int64 seedArg)` | Hash of a `String`'s UTF-8 bytes under an explicit `seedArg` |
| **One-shot statics (128-bit)** | |
| `static void hash128Into(int8[] data, int64 len, int64 seedArg, int8[] out16)` | 128-bit digest under `seedArg`, written as 16 bytes (low64 LE, then high64 LE) into `out16` — zero allocation when the buffer is reused |
| `static #int8[] hash128(int8[] data, int64 len)` | 128-bit digest (per-process seed) as a fresh 16-byte array (low64 LE, then high64 LE) |
| `static int64 hash128Low(int8[] data, int64 len)` | Low 64 bits of the 128-bit digest |
| `static int64 hash128High(int8[] data, int64 len)` | High 64 bits of the 128-bit digest |
| `static #String hash128Hex(int8[] data, int64 len)` ⚑ | 128-bit digest as the 32-char canonical lowercase hex string (big-endian high64 then low64) |
| `static #String hash128HexSeeded(int8[] data, int64 len, int64 seedArg)` | 128-bit canonical hex digest under an explicit `seedArg` |
| **Streaming** | |
| `XXHash3()` ⚑ | Construct a streaming hasher seeded with the per-process random seed |
| `XXHash3(int64 seedArg)` | Construct with an explicit seed — for digests that reproduce across processes |
| `void update(int8[] data, int64 len)` | Feed `data[0..len)` into the running hash state |
| `void reset()` | Discard accumulated input and re-seed from the original seed |
| `int64 finish()` | Finalize and return the 64-bit digest of everything written so far |
| **`Hasher` typed writes** | |
| `void writeBoolean(boolean v)` | Feed `v` as one byte: `1` for true, `0` for false |
| `void writeInt8(int8 v)` | Append the raw byte of `v` |
| `void writeInt16(int16 v)` | Append the 2 raw bytes of `v` |
| `void writeInt32(int32 v)` | Append the 4 raw bytes of `v` |
| `void writeInt64(int64 v)` | Append the 8 raw bytes of `v` |
| `void writeUInt8(uint8 v)` | Append the raw byte of `v` |
| `void writeUInt16(uint16 v)` | Append the 2 raw bytes of `v` |
| `void writeUInt32(uint32 v)` | Append the 4 raw bytes of `v` |
| `void writeUInt64(uint64 v)` | Append the 8 raw bytes of `v` |
| `void writeFloat32(float32 v)` | Append the 4 raw bytes of `v` |
| `void writeFloat64(float64 v)` | Append the 8 raw bytes of `v` |
| `void writeBytes(int8[] data)` | Feed every byte of `data` |
| `void writeBytesRange(int8[] data, int64 offset, int64 length)` | Feed `length` bytes of `data` (v1: `offset` is ignored) |
| `void writeString(String s)` | Feed a `String`'s UTF-8 bytes |
| `void writeObject(Object obj)` | Mix in `obj.hash()` as 8 bytes |
| **SIMD long-path building blocks** (public internals of the pure-cajeta long-input path; `hash()` routes inputs > 240 bytes here) | |
| `static #int8[] buildSecret(int64 seed)` | Build the 192-byte seeded secret (`XXH3_initCustomSecret`) |
| `static int64 mulFold(int64 a, int64 b)` | 64x64 → 128 multiply, folded: `low64(a*b) ^ high64(a*b)` |
| `static int64 mix2(int64 a0, int64 a1, int8[] secret, int64 off)` | Mix two accumulator lanes against the secret |
| `static int64 avalanche(int64 h)` | Final avalanche mix |
| `static int64 hashLong(int8[] data, int64 len, int64 seed)` | The long-input driver — bit-identical to the native `XXH3_64bits_withSeed` long path |
| `static void hashLong128Into(int8[] data, int64 len, int64 seed, int8[] out16)` | 128-bit long-input driver, writing 16 bytes into `out16` |

⚑ = `@EntryPoint`

## See also

- Tour: [HashDemo](../../../samples/tour/src/main/cajeta/tour/hash/HashDemo.cajeta)
- Source: [`runtime/src/cajeta/hash/XXHash3.cajeta`](../../../runtime/src/cajeta/hash/XXHash3.cajeta)
- [DefaultHasher](DefaultHasher.md) — the process-seeded wrapper `Object.hash()` uses, [SipHash](SipHash.md) — when the input is attacker-controlled, [Blake3](Blake3.md) — when you need a cryptographic hash
