# HmacSha1

`cajeta.hash.HmacSha1` — HMAC (RFC 2104) over [Sha1](Sha1.md). It exists for
one family of callers: the one-time password algorithms, HOTP and TOTP, whose
installed base of authenticator apps computes SHA-1 and ignores any other
algorithm. For a new keyed MAC use [HmacSha256](HmacSha256.md). The key is
padded or hashed to one 64-byte block at construction, so `reset()` restarts
a MAC under the same key.

```cajeta
int8[] key = heap int8[20];
int8[] msg = heap int8[8];
int8[] mac #= HmacSha1.mac(key, 20, msg, 8);            // 20 bytes
String hex #= HmacSha1.macHex(key, 20, msg, 8);         // 40 lowercase hex chars
```

## Methods

| Signature | |
|---|---|
| `static #int8[] mac(int8[] key, int64 keyLen, int8[] data, int64 len)` | 20-byte MAC of `data[0..len)` under `key[0..keyLen)` |
| `static #String macHex(int8[] key, int64 keyLen, int8[] data, int64 len)` | The same MAC as 40 lowercase hex characters |
| `HmacSha1(int8[] key, int64 keyLen)` | Construct under a key; a key longer than 64 bytes is hashed first |
| `void update(int8[] data, int64 len)` | Append `data[0..len)` to the message |
| `#int8[] digest()` | The 20-byte MAC of everything appended since construction or `reset()` |
| `#String hex()` | The same MAC as hex |
| `void reset()` | Restart under the same key |

## See also

- Source: [`runtime/src/cajeta/hash/HmacSha1.cajeta`](../../../runtime/src/cajeta/hash/HmacSha1.cajeta)
- [Totp](Totp.md) — the caller, [HmacSha256](HmacSha256.md) — the MAC for everything else
