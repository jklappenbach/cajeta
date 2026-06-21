---
id: lang-encoding
applies-to: [cajeta/lang/Encoding, cajeta/lang/EncodingErrorPolicy, cajeta/lang/EncodingException]
title: Encoding / EncodingErrorPolicy / EncodingException — the bytes↔text conversion trio
description: Name an Encoding + an EncodingErrorPolicy at every bytes↔text boundary; FAIL raises the recoverable EncodingException pinpointing the bad codepoint.
---

# The bytes ↔ text conversion trio

Every cajeta conversion across the bytes ↔ text boundary names **two** things
explicitly — *which* `Encoding`, and *what to do* on failure via
`EncodingErrorPolicy`. There is **no platform-default encoding** (deliberately
unlike Java's `FileReader`) and **no silent-loss IGNORE** policy. Under
`EncodingErrorPolicy.FAIL` an unrepresentable codepoint or undecodable byte
raises `EncodingException`, which is *recoverable* — catch it at the ingestion
boundary and fall back.

## Members and roles

- **`Encoding`** — `enum`, 12 constants, *which byte representation*. Ordinals are
  sequential from `UTF_8 = 0`: `UTF_8, UTF_16_LE, UTF_16_BE, UTF_32_LE,
  UTF_32_BE, ASCII, LATIN_1, WINDOWS_1252, GB18030, SHIFT_JIS, EUC_KR, BIG_5`.
  An `Encoding`-typed value *is* the ordinal (it arithmetic-promotes to `int32`).
- **`EncodingErrorPolicy`** — `enum`, *what happens on an unrepresentable unit*.
  `FAIL = 0` (default; throw) and `REPAIR = 1` (substitute the encoding's
  replacement marker — `U+FFFD` for UTF targets, `'?'` for ASCII/Latin-1/
  Windows-1252, codec-specified for CJK — and continue). No `IGNORE`.
- **`EncodingException`** — `class extends cajeta.error.RecoverableException`
  (so catchable; see `cajeta/error/RecoverableException`). The structured failure
  raised only under `FAIL`.

## Collaboration / how they cooperate

These three are a *vocabulary*, not a pipeline of objects that hold each other:
the caller picks one `Encoding` constant and one `EncodingErrorPolicy` constant
and hands both to a conversion method; that method (under `FAIL`) is the sole
thing that constructs and throws `EncodingException`. The exception records the
two encodings **as ordinals**, not as `Encoding`-typed fields:

```
Encoding + EncodingErrorPolicy.FAIL ──▶ conversion ──(bad unit)──▶ heap EncodingException
```

`EncodingException` fields (all public):

- `int64 offset` — source position of the bad unit. **Direction-dependent**:
  for text→bytes (`getBytes`) it is the *codepoint index* in the source String;
  for bytes→text (`fromBytes`) it is the *byte index* in the input buffer.
- `int32 codepoint` — the offending Unicode codepoint, or `0` when the failure is
  byte-side (malformed input bytes have no codepoint — read `reason` instead).
- `int32 sourceEncoding`, `int32 targetEncoding` — **`Encoding` ordinals**, not
  `Encoding`-typed. Compare against `Encoding.UTF_8` etc. (ordinal arithmetic).
- `String reason` — machine-friendly variant of the inherited `message`
  (`message` comes from `Throwable`); for structured diagnostic surfaces.

## NOT yet implemented — read this before hunting for a call site

The conversion methods that *consume* this trio (`String.getBytes`,
`String.fromBytes`, `FileReader` transcoding hooks) **do not exist yet** — see
`cajeta/lang/String` for the phase plan. Today you can only *name* the constants
and *construct/throw/catch* `EncodingException` directly. Do not search for a
`String.getBytes` to call; it isn't there. The crypto/compression family is also
not here — that lives under `cajeta.codec.*`.

## When to use FAIL vs REPAIR

- `FAIL` (default) — ingestion of data that *must* be valid; you want the bad
  position surfaced so you can reject or fix it. Catch `EncodingException`.
- `REPAIR` — best-effort display/logging where producing detectable output
  (`U+FFFD`/`?`) beats aborting. Never produces silent loss.
- Genuinely want characters *dropped*? There is no policy for it — decode-iterate
  and filter codepoints manually, then re-encode.

## Ownership / lifecycle

- Both enums are plain ordinal values — nothing to own, free, or close.
- `EncodingException` is allocated with `heap` and thrown; as a
  `RecoverableException` it propagates to the nearest matching `catch` rather than
  aborting the program. Its `String` fields follow normal cajeta exception
  ownership (the parent `RecoverableException(#String)` takes its message by `#`
  transfer).

## Worked example (mirrors test/parser/EncodingTypesTests.cpp)

```cajeta
import cajeta.lang.Encoding;
import cajeta.lang.EncodingErrorPolicy;
import cajeta.lang.EncodingException;

// Select the wire encoding and the failure policy up front.
Encoding wire = Encoding.ASCII;
EncodingErrorPolicy onBadData = EncodingErrorPolicy.FAIL;

// Until conversion methods land, the exception is what you exercise directly.
// This is exactly what a getBytes(UTF_8 -> ASCII) under FAIL would raise on 'é':
try {
    throw heap EncodingException(
        "codepoint U+1F600 not representable in ASCII",  // message (Throwable)
        (int64) 42,          // offset: codepoint index in the source String
        128512,              // codepoint: U+1F600
        Encoding.UTF_8,      // sourceEncoding ordinal (internal storage)
        Encoding.ASCII,      // targetEncoding ordinal (caller-chosen wire)
        "U+1F600 out of ASCII range");                   // reason
} catch (EncodingException e) {
    log(e.reason);                          // structured message
    log(e.offset);                          // 42
    log(e.codepoint);                       // 128512
    // ordinals: UTF_8 == 0, ASCII == 5
    log(e.sourceEncoding * 100 + e.targetEncoding);  // 5
}
```
