# SipHash

`cajeta.hash.SipHash` — SipHash-2-4, a 64-bit DoS-resistant keyed hash. Use
when the input is attacker-controlled and a hash-map or cache lookup depends
on it (HTTP request keys, JWT claim names, user-supplied cache keys): the
128-bit key prevents an attacker from precomputing colliding inputs because
they don't know the key. For internal, non-attacker inputs use
[XXHash3](XXHash3.md) instead — roughly 10x faster per byte.

```cajeta
int8[] data = heap int8[12];
int64 h = SipHash.hashKeyed(data, 12, 17, 42);   // one-shot, keyed

SipHash s = heap SipHash(17, 42);
s.writeString("user-key");
int64 digest = s.finish();
```

## Methods

| Signature | |
|---|---|
| **One-shot statics** | |
| `static int64 hashKeyed(int8[] data, int64 len, int64 keyLow, int64 keyHigh)` ⚑ | Keyed digest of `data[0..len)` — the common case, no streaming object required |
| `static int64 hashStringKeyed(String s, int64 keyLow, int64 keyHigh)` ⚑ | Keyed digest of a `String`'s UTF-8 bytes |
| **Streaming** | |
| `SipHash(int64 keyLow, int64 keyHigh)` ⚑ | Construct a streaming SipHash with the 128-bit key as two `int64` halves |
| `void update(int8[] data, int64 len)` | Absorb `data[0..len)` into the running digest |
| `void reset()` | Re-initialize the state from the stored key so the instance can be reused |
| `int64 finish()` | Finalize the streamed input and return the 64-bit digest |
| **`Hasher` typed writes** | |
| `void writeBoolean(boolean v)` | Absorb `v` as one byte: `1` for true, `0` for false |
| `void writeInt8(int8 v)` | Absorb the raw byte of `v` |
| `void writeInt16(int16 v)` | Absorb the 2 raw bytes of `v` |
| `void writeInt32(int32 v)` | Absorb the 4 raw bytes of `v` |
| `void writeInt64(int64 v)` | Absorb the 8 raw bytes of `v` |
| `void writeUInt8(uint8 v)` | Absorb the raw byte of `v` |
| `void writeUInt16(uint16 v)` | Absorb the 2 raw bytes of `v` |
| `void writeUInt32(uint32 v)` | Absorb the 4 raw bytes of `v` |
| `void writeUInt64(uint64 v)` | Absorb the 8 raw bytes of `v` |
| `void writeFloat32(float32 v)` | Absorb the 4 raw bytes of `v` |
| `void writeFloat64(float64 v)` | Absorb the 8 raw bytes of `v` |
| `void writeBytes(int8[] data)` | Absorb every byte of `data` |
| `void writeBytesRange(int8[] data, int64 offset, int64 length)` | Absorb `length` bytes of `data` (v1: `offset` is ignored) |
| `void writeString(String s)` | Absorb a `String`'s UTF-8 bytes |
| `void writeObject(Object obj)` | Absorb `obj.hash()` as 8 bytes |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/hash/SipHash.cajeta`](../../../runtime/src/cajeta/hash/SipHash.cajeta)
- [XXHash3](XXHash3.md) — much faster for non-attacker-controlled input
