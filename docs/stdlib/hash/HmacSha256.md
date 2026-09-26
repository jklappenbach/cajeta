# HmacSha256

`cajeta.hash.HmacSha256` — HMAC (RFC 2104) over [Sha256](Sha256.md), a keyed
message authentication code. It signs a JWT under HS256, authenticates a
cookie, and is the PRF inside [Pbkdf2](Pbkdf2.md). The key is padded or
hashed to one 64-byte block at construction, so `reset()` restarts a MAC
under the same key without touching the key again. Verify a MAC with
[Hash](Hash.md)`.constantTimeEquals`, never with a plain byte loop.

```cajeta
int8[] key = heap int8[16];
int8[] msg = heap int8[64];
int8[] mac #= HmacSha256.mac(key, 16, msg, 64);          // 32 bytes
String hex #= HmacSha256.macHex(key, 16, msg, 64);       // 64 lowercase hex chars

HmacSha256 h = heap HmacSha256(key, 16);
h.update(msg, 32);
h.update(msg, 64);
int8[] streamed #= h.digest();
h.reset();                                               // same key, fresh message
```

## Methods

| Signature | |
|---|---|
| **One-shot statics** | |
| `static #int8[] mac(int8[] key, int64 keyLen, int8[] data, int64 len)` | 32-byte MAC of `data[0..len)` under `key[0..keyLen)` |
| `static #String macHex(int8[] key, int64 keyLen, int8[] data, int64 len)` | The same MAC as 64 lowercase hex characters |
| `static #String toHex(int8[] d, int64 n)` | Lowercase hex of `d[0..n)` |
| **Streaming** | |
| `HmacSha256(int8[] key, int64 keyLen)` | Construct under a key; a key longer than 64 bytes is hashed first |
| `void update(int8[] data, int64 len)` | Append `data[0..len)` to the message |
| `#int8[] digest()` | The 32-byte MAC of everything appended since construction or `reset()` |
| `#String hex()` | The same MAC as hex |
| `void reset()` | Restart under the same key |

## See also

- Source: [`runtime/src/cajeta/hash/HmacSha256.cajeta`](../../../runtime/src/cajeta/hash/HmacSha256.cajeta)
- [Sha256](Sha256.md) — the digest underneath, [Pbkdf2](Pbkdf2.md) — password hashing built on this MAC, [Hash](Hash.md) — `constantTimeEquals` for checking a MAC
