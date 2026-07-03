# Sha1

`cajeta.hash.Sha1` — SHA-1 (FIPS 180-4), a 160-bit digest. **Not for security
use**: SHA-1's collision resistance is broken (practical collisions since
2017, chosen-prefix since 2020). The one sanctioned use in this tree is the
WebSocket opening handshake, where RFC 6455 fixes SHA-1 as a protocol
constant for the `Sec-WebSocket-Accept` value — the handshake depends on
SHA-1's correctness, not its security. For real cryptographic hashing use
[Sha256](Sha256.md).

```cajeta
int8[] data = heap int8[8];
String hex = Sha1.hashHex(data, 8);   // 40 lowercase hex chars

Sha1 h = heap Sha1();
h.update(data, 8);
int8[] digest = h.digest();           // 20 bytes
```

## Methods

| Signature | |
|---|---|
| **One-shot statics** | |
| `static #int8[] hash(int8[] data, int64 len)` ⚑ | 20-byte digest of `data[0..len)` |
| `static #String hashHex(int8[] data, int64 len)` ⚑ | Lowercase 40-char hex digest of `data[0..len)` |
| `static #int8[] hashString(String s)` | 20-byte digest of a `String`'s UTF-8 bytes |
| `static #String hashStringHex(String s)` | Lowercase hex digest of a `String`'s UTF-8 bytes |
| **Streaming** | |
| `Sha1()` ⚑ | Construct a fresh streaming hasher |
| `void update(int8[] data, int64 len)` | Append `data[0..len)` to the streaming digest |
| `#int8[] digest()` | Finalize and return the 20-byte digest |
| `void reset()` | Re-initialize the working state |
| `int64 finish()` | Project the 160-bit digest to its first 8 bytes as a little-endian `int64` (the `Hasher` contract); call `digest()` *before* `finish()` for the full 20 bytes |
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
| `void writeObject(Object obj)` | Mix in `obj.hash()` as 8 bytes |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/hash/Sha1.cajeta`](../../../runtime/src/cajeta/hash/Sha1.cajeta)
- [Sha256](Sha256.md) — the SHA-2 family member to use for actual cryptographic hashing
