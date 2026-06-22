---
id: codec-base64-Base64
applies-to: [cajeta/codec/Base64]
title: Base64 — RFC 4648 byte ⇄ text codec (static, one-shot)
description: Encode/decode bytes to Base64 text and back; standard and URL-safe alphabets, owned returns, Base64Exception on bad input.
---

# Base64

RFC 4648 codec in `cajeta.codec`. **Access point — call statically; never construct.**
`Base64` is a `final` class with only static methods; there is no instance and no state.

Pick by task:

| You have / want | Call |
| --- | --- |
| bytes → standard text, `=`-padded (§4) | `Base64.encode(data, len)` |
| bytes → URL/filename-safe text, **no** padding (§5; JWT, URL params) | `Base64.encodeUrlSafe(data, len)` |
| bytes → text, choose alphabet + padding | `Base64.encode(data, len, urlSafe, pad)` |
| text `String` → bytes | `Base64.decode(s)` |
| encoded text already held as a byte buffer → bytes | `Base64.decodeBytes(data, len)` |

Decode is **alphabet-agnostic**: it accepts `+`/`/` and `-`/`_` interchangeably, padded
or unpadded — you do not need to know which variant produced the text.

## Signatures, ownership & errors

```
static #String encode(int8[] data, int64 len)
static #String encodeUrlSafe(int8[] data, int64 len)
static #String encode(int8[] data, int64 len, boolean urlSafe, boolean pad)
static #int8[] decode(String s)
static #int8[] decodeBytes(int8[] data, int64 len)
```

- **Returns are owned (`#`)** — a fresh `String` / `int8[]` the caller owns and must
  transfer or drop. The input `data`/`s` is **borrowed** (read-only, not retained).
- `len` is the number of bytes to read from `data` (pass `data.count()` for the whole
  array); a negative `len` is clamped to 0. `decode(s)` reads `s.byteLength` bytes of
  `s.bytes` for you.
- **Errors:** decode throws [[Base64Exception]] (a `RecoverableException`, so catch or
  declare it) on a byte outside `[A-Za-z0-9+/-_]` (other than `=`), or a padding-stripped
  length of `1 mod 4` ("truncated final quantum"). Its `position` field is the 0-based
  byte offset into the *encoded* input where the problem was noticed. **Encode never throws.**

## Example

```cajeta
import cajeta.codec.Base64;
import cajeta.codec.Base64Exception;
import cajeta.lang.String;

// Encode — owned String, returned via transfer.
public static #String toB64(int8[] bytes) {
    return Base64.encode(bytes, bytes.count());
}

// Decode — owned int8[]. A fresh return must carry `#` on the
// signature AND be returned with the transfer operator, or the
// borrow checker rejects it (CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER).
public static #int8[] fromB64(String text) {
    int8[] raw = Base64.decode(text);   // may throw Base64Exception
    return #raw;
}
```

To recover from bad input, wrap the decode in `try { … } catch (Base64Exception e) { … }`.

## What it does NOT do

- **No streaming / incremental** API — these are one-shot whole-buffer calls only.
- **No MIME line wrapping** — output is a single unbroken run; it never inserts the
  76-char `\r\n` breaks of RFC 2045, and decode does not skip embedded whitespace or
  newlines (they are invalid characters and throw).
- **No padding validation** — decode merely strips trailing `=`; it does not check that
  the pad count is correct, and unpadded URL-safe input round-trips without you re-adding `=`.
- No `@Native` bridge — pure Cajeta index arithmetic; no static alphabet table dependency.
