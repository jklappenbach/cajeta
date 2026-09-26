# Base32

`cajeta.codec.Base32` — Base32 (RFC 4648 §6) byte ⇄ text codec, the alphabet
`A`–`Z` `2`–`7`. Its one everyday use is the shared secret in an `otpauth://`
enrollment URI, which authenticator apps read and people sometimes type, so
decoding accepts lowercase and tolerates missing `=` padding. A character
outside the alphabet or a length no byte sequence produces raises
`Base32Exception` with the offending byte offset.

```cajeta
int8[] secret = heap int8[20];
String padded #= Base32.encode(secret, (int64) secret.count());          // 32 chars, no '=' at 20 bytes
String bare #= Base32.encode(secret, (int64) secret.count(), false);     // never padded
int8[] raw #= Base32.decode("mzxw6ytboi");                               // "foobar"
```

## Methods

| Signature | |
|---|---|
| `static #String encode(int8[] data, int64 len)` | Encode `data[0..len)` with `=` padding to a multiple of 8 |
| `static #String encode(int8[] data, int64 len, boolean pad)` | Encode with padding chosen explicitly |
| `static #int8[] decode(String s)` | Decode upper or lower case, padded or not; throws `Base32Exception` on garbage |
| `static #int8[] decodeBytes(int8[] data, int64 len)` | Decode `data[0..len)` held as bytes |

## See also

- [Base64](Base64.md) — the codec for everything that is not an OTP secret
- Source: [`runtime/src/cajeta/codec/Base32.cajeta`](../../../runtime/src/cajeta/codec/Base32.cajeta)
