# Base64

`cajeta.codec.Base64` — Base64 (RFC 4648) byte ⇄ text codec. Supports both
alphabets in §4 (standard, `+` `/`) and §5 (URL- and filename-safe, `-` `_`),
with explicit control over padding. Decoding accepts either alphabet
transparently, so a caller need not know which variant produced the text (an
HTTP `Authorization: Basic …` header vs. a JWT segment). A character outside
the alphabet or a stripped length of `1 mod 4` raises `Base64Exception` with
the offending byte offset; trailing `=` padding is consumed and ignored, so
unpadded URL-safe input round-trips without the caller re-adding `=`.

```cajeta
int8[] data = heap int8[3];
data[0] = (int8) 77;
data[1] = (int8) 97;
data[2] = (int8) 110;
String b = Base64.encode(data, (int64) data.count());        // "TWFu"
String u = Base64.encodeUrlSafe(data, (int64) data.count()); // "TWFu" (no '=')
int8[] raw = Base64.decode(b);       // either alphabet, padded or not
```

## Methods

| Signature | |
|---|---|
| `static #String encode(int8[] data, int64 len)` ⚑ | Encode `data[0..len)` to standard Base64 (§4) with `=` padding |
| `static #String encodeUrlSafe(int8[] data, int64 len)` ⚑ | Encode to URL-/filename-safe Base64 (§5, `-`/`_`) without padding — the JWT / query-parameter form |
| `static #String encode(int8[] data, int64 len, boolean urlSafe, boolean pad)` | General encode: pick the alphabet and padding explicitly |
| `static #int8[] decode(String s)` ⚑ | Decode a Base64 `String` (either alphabet, padded or not) to raw bytes; throws `Base64Exception` on garbage |
| `static #int8[] decodeBytes(int8[] data, int64 len)` | Decode `data[0..len)` (encoded text held as bytes) to raw bytes |

⚑ = `@EntryPoint`

## See also

- [Json](json/Json.md), [Csv](csv/Csv.md) — the other `cajeta.codec` codecs
- Source: [`runtime/src/cajeta/codec/Base64.cajeta`](../../../runtime/src/cajeta/codec/Base64.cajeta)
