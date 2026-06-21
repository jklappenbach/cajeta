---
id: lang-guid-parse
applies-to: [cajeta/lang/Guid.parse]
title: Guid.parse — canonical 8-4-4-4-12 hex text to an owned Guid
description: Parse a 36-char 8-4-4-4-12 hex UUID into a #Guid; consumes the #String input, returns owned, raises recoverable GuidFormatException on any malformed input.
---

# Guid.parse(#String) — text to a 128-bit Guid

```cajeta
public static #Guid parse(#String s)
```

Use when you have canonical UUID *text* and need a `Guid` value. Input ownership
transfers in (`#String`); the returned `#Guid` is heap-allocated and **owned by
the caller**. On any malformed input it throws `GuidFormatException` (a
`RecoverableException`, see `cajeta/lang/GuidFormatException`) — catch it at the
parse boundary and reject the input; it does not abort.

## What it accepts — and what it does NOT

- **Accepts** exactly the canonical 36-char `8-4-4-4-12` form, e.g.
  `01234567-89ab-cdef-fedc-ba9876543210`. Hex digits may be **upper or
  lowercase** (round-tripping through `Guid.toString()` normalizes to lowercase).
- **Does NOT** accept the 32-char unhyphenated form, brace-wrapped `{...}`,
  the `urn:uuid:` prefix, or surrounding whitespace — there is no trimming or
  normalization. Anything but length-36-with-hyphens-at-the-canonical-spots is
  rejected.
- **Does NOT** validate version or variant bits. Any 36-char hex string in the
  right shape parses, even if it isn't a real RFC 4122 v4 GUID. (For a *generated*
  v4 value use `Guid.random()`, not parse.)

## Parameters & failure modes

`s` — the UUID text, ownership transferred (`#`). It is read byte-wise via
`String.byteAt`; separators must be `'-'` at indices **8, 13, 18, 23**.

Throws `GuidFormatException` (caught, not fatal) when:

- `s` is `null` **or** `s.byteLength != 36` — wrong length is checked first, so
  null and short/long inputs never reach digit parsing (no null-deref).
- a separator position (8/13/18/23) is not `'-'`.
- any other position is not a hex digit (`0-9 a-f A-F`).

No partial result and no side effects: it builds a `uint128` accumulator and
either returns a fresh `heap Guid(acc)` or throws.

## Worked example (mirrors test/parser/GuidTests.cpp)

```cajeta
import cajeta.lang.Guid;
import cajeta.lang.GuidFormatException;
import cajeta.lang.String;

// Happy path: parse → toString is identity on canonical lowercase text.
Guid g = Guid.parse("01234567-89ab-cdef-fedc-ba9876543210");
// g is owned here; uppercase input would normalize to lowercase on toString().

// Untrusted input: reject at the boundary instead of crashing.
try {
    Guid id = Guid.parse(userInput);   // userInput: #String, consumed
    use(#id);
} catch (GuidFormatException e) {
    // wrong length, misplaced '-', or non-hex digit — reject userInput
}
```

For construction from raw bits instead of text, or for the inverse rendering, see
the `cajeta/lang/Guid` class skill (`of`, `fromHalves`, `random`, `toString`).
