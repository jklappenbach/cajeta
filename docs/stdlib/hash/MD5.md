# MD5

`cajeta.hash.MD5` — MD5 (RFC 1321), a 128-bit non-cryptographic checksum /
identifier. Legacy interop only: MD5 is cryptographically broken and the
slowest hash in the library, so keep it only where an external protocol fixes
the algorithm (S3 `Content-MD5` / multipart ETags, existing checksums you must
reproduce). For a 128-bit fingerprint in new code use `XXHash3.hash128`; for a
cryptographic digest use [Sha256](Sha256.md) or [Blake3](Blake3.md).

```cajeta
int8[] data = heap int8[16];
String etag = MD5.hashHex(data, 16);   // 32 lowercase hex chars

MD5 m = heap MD5();
m.writeString("record");
int8[] digest = m.digest();            // 16 bytes
```

## Methods

| Signature | |
|---|---|
| **One-shot statics** | |
| `static #int8[] hash(int8[] data, int64 len)` ⚑ | 16-byte digest of `data[0..len)` |
| `static #String hashHex(int8[] data, int64 len)` ⚑ | Lowercase 32-char hex digest of `data[0..len)` |
| `static #int8[] hashString(String s)` | 16-byte digest of a `String`'s UTF-8 bytes |
| `static #String hashStringHex(String s)` | Lowercase hex digest of a `String`'s UTF-8 bytes |
| **Streaming** | |
| `MD5()` ⚑ | Create an empty streaming hasher |
| `void update(int8[] data, int64 len)` | Append `data[0..len)` to the streaming digest |
| `#int8[] digest()` | Finalize and return the 16-byte digest |
| `void reset()` | Re-initialize the working state |
| `int64 finish()` | Finalize and project the 128-bit digest to its first 8 bytes as a little-endian `int64` (the `Hasher` contract); call `digest()` *before* `finish()` if you need the full 16 bytes |
| **`Hasher` typed writes** | |
| `void writeBoolean(boolean v)` | Append `v` as one byte: `1` for true, `0` for false |
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
| `void writeBytes(int8[] data)` | Append every byte of `data` |
| `void writeBytesRange(int8[] data, int64 offset, int64 length)` | Append `length` bytes of `data` (v1: `offset` is ignored) |
| `void writeString(String s)` | Append a `String`'s UTF-8 bytes |
| `void writeObject(Object obj)` | Mix in `obj.hash()` (the object's 64-bit hash) — not the object's deep byte contents |

⚑ = `@EntryPoint`

## See also

- Tour: [HashDemo](../../../samples/tour/src/main/cajeta/tour/hash/HashDemo.cajeta)
- Source: [`runtime/src/cajeta/hash/MD5.cajeta`](../../../runtime/src/cajeta/hash/MD5.cajeta)
- [XXHash3](XXHash3.md) — `hash128` is the direct MD5 replacement for fingerprints, [Sha256](Sha256.md) / [Blake3](Blake3.md) — for cryptographic digests
