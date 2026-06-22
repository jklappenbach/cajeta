---
id: codec-json-JsonReader-next
applies-to: [cajeta/codec/json/JsonReader.next]
title: JsonReader.next — pull-tokenizer token pump
description: Advance JsonReader one token; drive to JsonToken.END, materialize spans before re-advancing, JsonParseException on malformed input.
---

# JsonReader.next

`public JsonToken next()` — advance the cursor one token forward and return its
kind. This is the pump of the Tier-2 streaming parser: call it in a loop until it
returns `JsonToken.END`. The returned `JsonToken` is a plain enum value (no
ownership, never null). `next()` itself **allocates nothing** — string/number
tokens are recorded as byte-offset *spans* into the input buffer; you pay the
decode/allocation cost only when you ask for a concrete value.

## The protocol — drive to END

```cajeta
import cajeta.codec.json.JsonReader;
import cajeta.codec.json.JsonToken;
import cajeta.lang.String;

String src = "{\"id\":42,\"active\":true}";
stack reader = JsonReader(src.bytes, (int64) src.byteLength);

JsonToken t = reader.next();
while (t != JsonToken.END) {
    if (t == JsonToken.KEY) {
        #String key = reader.currentString();   // owned copy — see below
    } else if (t == JsonToken.NUMBER) {
        int64 n = reader.currentNumberAsInt64();
    } else if (t == JsonToken.BOOLEAN) {
        boolean b = reader.currentBoolean();    // BOOLEAN is one token
    } else if (t == JsonToken.STRING) {
        #String s = reader.currentString();
    }
    t = reader.next();
}
```

Token sequence for `{"id":42}`: `START_OBJECT`, `KEY`, `NUMBER`, `END_OBJECT`,
then `END`. Inside an **object** frame, each value is preceded by its own `KEY`
token; after a `KEY`, the very next `next()` yields that value and is *not*
separator-gated. `START_ARRAY`/`START_OBJECT` push a frame; the matching
`END_ARRAY`/`END_OBJECT` pops it.

## Side effects (mutates the receiver)

Each call advances `pos` and overwrites the token-bounds fields
`tokenStart`/`tokenEnd` (byte offsets of the current span), plus `current` and,
for `BOOLEAN`, `currentBool`. It also mutates the internal depth stack and the
after-key flag, and consumes a cached `peek()`.

It does **not**: decode string escapes, parse numbers, allocate, validate UTF-8,
or skip BOM. It does **not** support comments or trailing commas (strict RFC
8259). To skip a whole value/subtree without decoding, use `skipValue()`; to
materialize a subtree into a DOM, use `readValue()`.

## Span lifetime — materialize before you re-advance

`tokenStart`/`tokenEnd` (and thus `currentString()` / `currentBytes()` /
`currentNumberAs*` / `currentBoolean()`) describe the **current** token only.
The next `next()` overwrites them. If you need a token's value, read it *before*
calling `next()` again.

- `currentString()` / `currentBytes()` return a **fresh owned copy** (`#` —
  ownership transfers to you; the reader keeps nothing). String escapes are
  verbatim, not decoded (`\n` stays as the two bytes `\`, `n`).
- The input buffer is **borrowed**: the constructor takes `int8[] input` without
  `#`, so the caller still owns it and it must outlive the reader. `next()`
  reads from it but never frees it.

## peek() interaction

`peek()` calls `next()` once and caches the result; the cached token is returned
by the *next* `next()` call without re-advancing the buffer. So `peek()` does
move `pos` — don't read raw `pos` between a `peek()` and its consuming `next()`
expecting the pre-peek position.

## Failure modes — JsonParseException

Throws `JsonParseException` (a `cajeta.error.RecoverableException` carrying the
0-based byte `position` of the fault) on malformed structure:

- `"unexpected character"` — a byte that can't begin a value.
- `"expected ',' or container close"` — missing separator between items.
- `"unexpected end of input inside container"` — buffer exhausted while
  `depth != 0` (i.e. an unclosed `{`/`[`). At the top level, exhaustion instead
  returns `JsonToken.END` cleanly.
- `"unexpected end of input after separator"`, `"unterminated string"`,
  `"truncated literal"` / `"invalid literal"` for `true`/`false`/`null`.
- `"nesting depth limit exceeded"` — fixed limit of 1024 frames.
- `"expected ':' after object key"` / `expected '"' to begin object key` from the
  key scan.

After any throw the reader's state is **undefined** — discard it and re-parse
from a fresh source; do not call `next()` again on it.

See the class skill `cajeta/codec/json/JsonReader` for construction and the full
materialization-helper set, and `JsonToken` for the token kinds. For the
allocation-free whole-subtree skip see `JsonReader.skipValue`; for DOM
expansion see `JsonReader.readValue`.
