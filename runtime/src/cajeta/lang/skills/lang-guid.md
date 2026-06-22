---
id: lang-guid
applies-to: [cajeta/lang/Guid]
title: Guid — uint128-backed UUID value type
description: Create, parse, format, and compare 128-bit UUIDs with Guid (of/fromHalves/random/parse, value/high/low, equals, toString).
---

# Guid

A 128-bit globally-unique identifier (UUID) backed by a single `uint128` field, so
comparison, hashing, and storage are single-word operations — not a struct of two
longs or a byte array. **Immutable**: `value` is set once at construction.

`Guid` is a **support / value type**, not an access point you wire other classes
through — you make one (or receive one), read its bits, format it, compare it.
Lives in package `cajeta.lang`, extends `cajeta.lang.Object`.

## Construction & ownership

You do not normally call `new Guid(...)` directly — the public constructor exists
(`Guid(uint128)`) but the four **static factories all return an owned `#Guid`** (heap
allocation, ownership transfers to the caller):

| Want                                   | Call                                  |
| -------------------------------------- | ------------------------------------- |
| Fresh random (RFC 4122 **v4**) UUID    | `Guid.random()`                       |
| From canonical `8-4-4-4-12` hex text   | `Guid.parse(#String)` → may throw     |
| From two 64-bit halves (e.g. DB cols)  | `Guid.fromHalves(uint64 hi, uint64 lo)` |
| Box a raw `uint128` you already have   | `Guid.of(uint128)`                    |

`random()` seeds v4 entropy from `/dev/urandom` when available and sets the version /
variant bits. `fromHalves` treats `hi` as the most-significant 64 bits.

## The methods that matter

- `uint128 value()` — the raw 128-bit value (by value).
- `uint64 high()` — most-significant 64 bits.
- `uint64 low()` — least-significant 64 bits.
- `boolean equals(Guid other)` — **exact** 128-bit comparison; null-safe (returns
  `false` for a null argument).
- `#String toString()` — canonical **lowercase** `8-4-4-4-12` hex (36 chars). Returns
  an owned `#String`. Uppercase input to `parse` is normalized to lowercase here.
- `int64 hash()` — folds all 128 bits into 64. `==` and hash-keyed collections route
  through this and so carry the standard ~2⁻⁶⁴ collision caveat — **call `equals()`
  when you need certainty.**

## Errors

`parse` throws `#GuidFormatException` (a `RecoverableException` — see
`cajeta/lang/GuidFormatException`) when input is not exactly 36 chars, has a misplaced
`-` separator, or contains a non-hex digit. Catch it at the parse boundary. The other
factories never throw. `equals(null)` is `false`, not a throw.

## What it does NOT do

- No string accepted other than the canonical 36-char `8-4-4-4-12` form — no braces,
  no URN `urn:uuid:` prefix, no 32-char unhyphenated form.
- No versions other than v4 are generated (`random()` is v4 only).
- `toString` only emits lowercase; there is no uppercase or alternate-format option.

## Example

```cajeta
import cajeta.lang.Guid;
import cajeta.lang.GuidFormatException;
import cajeta.lang.String;

// Parse → toString round-trips on canonical lowercase text.
Guid g = Guid.parse("01234567-89ab-cdef-fedc-ba9876543210");
String text = g.toString();   // owned #String: "01234567-89ab-cdef-fedc-ba9876543210"

// Raw halves round-trip; high half is most-significant.
Guid h = Guid.fromHalves((uint64) 255L, (uint64) 1L);
// h.high() == 255, h.low() == 1, h.toString() == "00000000-0000-00ff-0000-000000000001"

// equals() is exact (use it, not ==/hash, when correctness matters).
Guid a = Guid.parse("01234567-89ab-cdef-fedc-ba9876543210");
Guid b = Guid.parse("01234567-89ab-cdef-fedc-ba9876543210");
boolean same = a.equals(b);   // true

// Reject malformed input at the boundary.
try {
    Guid bad = Guid.parse(userInput);
} catch (GuidFormatException e) {
    // userInput was not a well-formed GUID
}

Guid id = Guid.random();      // fresh v4
```
