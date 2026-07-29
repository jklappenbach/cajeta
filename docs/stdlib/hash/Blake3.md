# Blake3

`cajeta.hash.Blake3` — BLAKE3, a fast modern cryptographic hash with a
built-in extendable output function (XOF). The recommended hash for new
cryptographic fingerprinting and content-addressing: collision-resistant like
SHA-256 but much faster. The 256-bit (32-byte) digest is the default;
`hashXof` fills a buffer of any length. For non-cryptographic fingerprints
(cache keys, ETags where attacker control is impossible) prefer
[XXHash3](XXHash3.md) — faster still.

```cajeta
int8[] data = heap int8[16];
String hex = Blake3.hashHex(data, 16);   // 64 lowercase hex chars

Blake3 h = heap Blake3();
h.writeString("hello");
int8[] digest = h.digest();              // 32 bytes
```

## Methods

| Signature | |
|---|---|
| **One-shot statics** | |
| `static #int8[] hash(int8[] data, int64 len)` ⚑ | 32-byte digest of `data[0..len)` |
| `static void hashInto(int8[] data, int64 len, int8[] out32)` | 32-byte digest written into a caller-provided `out32` — zero allocation when the buffer is reused |
| `static #String hashHex(int8[] data, int64 len)` ⚑ | Lowercase 64-char hex digest of `data[0..len)` |
| `static #int8[] hashString(String s)` | 32-byte digest of a `String`'s UTF-8 bytes |
| `static #String hashStringHex(String s)` | Lowercase hex digest of a `String`'s UTF-8 bytes |
| `static void hashXof(int8[] data, int64 len, int8[] out)` | Extendable output: fill all `out.count()` bytes with XOF output of `data[0..len)` |
| **Streaming** | |
| `Blake3()` ⚑ | Construct a fresh streaming hasher |
| `void update(int8[] data, int64 len)` | Feed `data[0..len)` into the running hash state |
| `#int8[] digest()` | Finalize and return the 32-byte digest of everything written so far |
| `void reset()` | Discard accumulated input so the instance can be reused |
| `int64 finish()` | `Hasher` projection: first 8 digest bytes as a little-endian `int64` — for the full cryptographic digest use `digest()` / `hashHex` |
| **`Hasher` typed writes** | |
| `void writeBoolean(boolean v)` | Feed `v` as one byte: `1` for true, `0` for false |
| `void writeInt8(int8 v)` | Feed the raw byte of `v` |
| `void writeInt16(int16 v)` | Feed the 2 raw bytes of `v` |
| `void writeInt32(int32 v)` | Feed the 4 raw bytes of `v` |
| `void writeInt64(int64 v)` | Feed the 8 raw bytes of `v` |
| `void writeUInt8(uint8 v)` | Feed the raw byte of `v` |
| `void writeUInt16(uint16 v)` | Feed the 2 raw bytes of `v` |
| `void writeUInt32(uint32 v)` | Feed the 4 raw bytes of `v` |
| `void writeUInt64(uint64 v)` | Feed the 8 raw bytes of `v` |
| `void writeFloat32(float32 v)` | Feed the 4 raw bytes of `v` |
| `void writeFloat64(float64 v)` | Feed the 8 raw bytes of `v` |
| `void writeBytes(int8[] data)` | Feed every byte of `data` |
| `void writeBytesRange(int8[] data, int64 offset, int64 length)` | Feed `length` bytes of `data` (v1: `offset` is ignored) |
| `void writeString(String s)` | Feed a `String`'s UTF-8 bytes |
| `void writeObject(Object obj)` | Feed `obj.hash()` as 8 bytes |

⚑ = `@EntryPoint`

## See also

- Tour: [HashDemo](../../../samples/tour/src/main/cajeta/tour/hash/HashDemo.cajeta)
- Source: [`runtime/src/cajeta/hash/Blake3.cajeta`](../../../runtime/src/cajeta/hash/Blake3.cajeta)
- [Sha256](Sha256.md) — the SHA-2 cryptographic digest, [XXHash3](XXHash3.md) — faster non-cryptographic fingerprints
