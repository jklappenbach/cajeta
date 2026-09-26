# Pbkdf2

`cajeta.hash.Pbkdf2` — PBKDF2 (RFC 8018) with [HmacSha256](HmacSha256.md) as
the pseudo-random function. It turns a password and a salt into a key of any
length, at a cost set by the iteration count. Store the salt and the count
beside the hash, so the count can rise later without stranding existing
users. Draw the salt from [SecureRandom](SecureRandom.md).

```cajeta
int8[] password = heap int8[12];
int8[] salt #= SecureRandom.bytes(16);
int8[] key #= Pbkdf2.sha256(password, 12, salt, 16, 600000, 32);
```

## Methods

| Signature | |
|---|---|
| `static #int8[] sha256(int8[] password, int64 passwordLen, int8[] salt, int64 saltLen, int32 iterations, int32 length)` | `length` derived bytes from `password[0..passwordLen)` and `salt[0..saltLen)` after `iterations` rounds |

## See also

- Source: [`runtime/src/cajeta/hash/Pbkdf2.cajeta`](../../../runtime/src/cajeta/hash/Pbkdf2.cajeta)
- [HmacSha256](HmacSha256.md) — the PRF, [SecureRandom](SecureRandom.md) — where the salt comes from
