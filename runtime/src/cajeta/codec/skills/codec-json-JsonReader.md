---
id: codec-json-JsonReader
applies-to: [cajeta/codec/json/JsonReader]
title: JsonReader — streaming JSON pull tokenizer
description: Drive next()/peek() over an int8[] buffer; lazy span tokens, borrowed vs owned reads, number coercions, JsonParseException.
---

# JsonReader

**Access point — you construct this.** A forward-only pull tokenizer for RFC 8259 JSON
in `cajeta.codec.json` (the *streaming* half of the package; the DOM half is `JsonValue`
+ `JsonObject` + `JsonArray`). It scans an `int8[]` buffer once, emitting one
`JsonToken` per `next()`. String/number tokens are recorded as byte-offset *spans* into
your buffer — nothing is materialized until you ask, so the parse/decode/allocate cost is
pay-per-value.

Use `JsonReader` for the hot path (you dispatch on tokens yourself, skip what you don't
need). For a convenience tree, call `readValue()` here or use `Json.parse(...)` and walk
the resulting `JsonValue`.

## Construction & ownership

```cajeta
import cajeta.codec.json.JsonReader;
import cajeta.codec.json.JsonToken;
import cajeta.codec.json.JsonParseException;
import cajeta.lang.String;

stack reader = JsonReader(buf, buf.count());   // buf is int8[]
```

`JsonReader(int8[] input, int64 byteCount)` — `byteCount` is passed explicitly (the
array `.count()` property accessor hasn't landed; pass `buf.count()`). **The input array
is borrowed, not transferred** — there is no `#` on the parameter. The reader holds the
reference and reads from it for its whole life, so the buffer must outlive the reader and
must not be mutated while parsing. The reader does *not* free it. The reader internally
heap-allocates two 1024-entry depth stacks (max nesting depth 1024); they drop with the
reader.

The cursor starts *before* the first token — call `next()` to advance to it.

## The protocol — next / peek / current

`next() -> JsonToken` advances and returns the next token; returns `JsonToken.END` at top
level once the buffer is exhausted. Drive it to `END`:

```cajeta
JsonToken t = reader.next();
while (t != JsonToken.END) {
    if (t == JsonToken.NUMBER) {
        int64 n = reader.currentNumberAsInt64();
    } else if (t == JsonToken.STRING) {
        #String s = reader.currentString();
    } else if (t == JsonToken.BOOLEAN) {
        boolean b = reader.currentBoolean();   // BOOLEAN is one token; this disambiguates
    }
    t = reader.next();
}
```

`peek() -> JsonToken` is one-token lookahead: it advances the buffer once and caches the
token; repeated `peek()` calls are idempotent and return the same token; the next `next()`
returns the cached token without re-advancing. Use it to dispatch on an element's leading
token while leaving it in place for a recursive value-builder (e.g. peek `END_ARRAY` to
exit a loop vs `START_OBJECT` to hand off).

Accessors describe the *current* token (valid after the `next()`/`peek()` that produced
it): `current() -> JsonToken`, `currentBoolean() -> boolean`, `position() -> int64`,
`tokenStart()` / `tokenEnd() -> int64` (span bounds into your buffer). Token kinds:
`START_OBJECT`, `END_OBJECT`, `START_ARRAY`, `END_ARRAY`, `KEY`, `STRING`, `NUMBER`,
`BOOLEAN`, `NULL`, `END` (see `cajeta/codec/json/JsonToken`).

## Reading values — borrowed span vs #owned copy

The span getters `tokenStart()`/`tokenEnd()` are **borrowed views** into the input buffer
— valid only until you mutate/free `buf`; copy out to keep. The materializers all return
**fresh #owned** allocations you must drop:

- `currentBytes() -> #int8[]` — a fresh copy of the token's bytes. STRING/KEY are the
  *inner* bytes (no surrounding quotes), and **escapes are NOT decoded** — `\n` stays as
  the two bytes `\`,`n`. There is no escape-decoding API yet.
- `currentRawBytes() -> #int8[]` — like `currentBytes()` but *includes* the wire
  delimiters: for STRING/KEY the surrounding `"` quotes; identical to `currentBytes()` for
  NUMBER/BOOLEAN/NULL. For round-tripping through `JsonWriter.writeRaw` (@JsonRaw). Does
  NOT capture object/array subtrees — primitive token values only.
- `currentString() -> #String` — wraps a fresh `currentBytes()` copy in a
  `cajeta.lang.String`; same no-escape-decode caveat. Convenience over the hot path.

## Number coercions

NUMBER tokens are spans; parse them with these (each reparses the current token's span):

- `currentNumberAsInt64() -> int64` — **integers only**. A fractional/exponent form
  throws `JsonParseException` ("non-integer number"); out-of-range throws ("number out of
  int64 range"). Handles `INT64_MIN` correctly.
- `currentNumberAsInt32() -> int32` — delegates to int64 then narrows; **no range check
  on the narrowing yet** (caller's responsibility).
- `currentNumberAsFloat64() -> float64` — sign, integer part, `.`fraction, `e`/`E`
  exponent; naive accumulation, no strtod-class accuracy guarantee.

Note: `readValueAfter`'s NUMBER arm stores every number as int64 — the DOM does not retain
floats through `readValue()`; use `currentNumberAsFloat64()` on the token directly for
float fidelity.

## Building a DOM

```cajeta
stack reader = JsonReader(buf, buf.count());
#JsonValue root = reader.readValue();   // recursively expands objects/arrays; #owned tree
```

`readValue() -> #JsonValue` pulls one token and builds the value subtree; ownership
transfers to the caller (its drop chain reclaims it). `readValueAfter(JsonToken t) ->
#JsonValue` builds from an already-pulled leading token (use inside a loop that already
called `next()`). See the `JsonValue`/`JsonObject`/`JsonArray` skills for navigation.

## skipValue

`skipValue()` consumes one complete value (scalar or fully-nested container) **without
materializing or allocating** for the skipped bytes — the on-demand skip used by the
`Json.parse<T>` synthesizer for unmapped keys. Throws `JsonParseException` if input ends
mid-value.

## Errors & lifecycle

All malformed-input failures throw `heap JsonParseException` (a `RecoverableException`)
carrying `.position`, the 0-based byte offset of the fault — bad separator, unexpected
character, truncated/unterminated string or literal, unterminated container, depth >1024,
or a bad number coercion. **After any throw the reader's state is undefined** — discard it
and restart from a fresh source; do not keep calling `next()`. There is no `close()` and
nothing else to dispose; the reader drops on scope exit (it never owns the input buffer).

## What it does NOT do

- No escape decoding (`currentBytes`/`currentString` give verbatim bytes).
- No relaxed JSON — strict RFC 8259, no comments or trailing commas.
- No float storage in the `readValue()` DOM (numbers become int64 there).
- No reverse/seek/reset — forward-only, single pass.
- Does not own or free the input `int8[]`; does not copy it on construction.
