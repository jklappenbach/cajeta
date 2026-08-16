---
id: codec-json-streaming
applies-to: [cajeta/codec/json/JsonReader, cajeta/codec/json/JsonWriter, cajeta/codec/json/JsonToken]
title: JSON streaming — pull-parse with JsonReader, emit with JsonWriter
description: Tier-2 hot-path JSON — drive JsonReader.next() over JsonToken kinds and build output with JsonWriter's fluent builder, no DOM tree.
---

# JSON streaming (pull reader + builder writer)

Use this when you want to parse or emit JSON **without** materializing a
`JsonValue` tree: the reader pumps one `JsonToken` per `next()` call over a
borrowed `int8[]` buffer; the writer accretes wire bytes through a fluent
chain. This is the Tier-2 path — lower allocation, more caller bookkeeping.

For random-access / "load the whole document and navigate it" use the **DOM
component** instead (`Json.parse<T>`, `JsonReader.readValue()` → `JsonValue` /
`JsonObject` / `JsonArray`). `readValue()` lives on `JsonReader` but produces a
DOM tree — see the JsonValue/JsonObject/JsonArray skills for navigating it.

## Members and roles

- **`JsonToken`** — enum of token kinds returned by `JsonReader.next()`:
  `START_OBJECT END_OBJECT START_ARRAY END_ARRAY KEY STRING NUMBER BOOLEAN NULL END`.
  Ordinals are `0..9` in that order (tests compare against the ints). `BOOLEAN`
  is one token; the reader's `currentBoolean()` says true vs false.
- **`JsonReader`** — pull tokenizer. Walks an `int8[]` once forward. String and
  number tokens are recorded as byte-offset *spans* (`tokenStart`/`tokenEnd`);
  you pay the decode cost only when you call a `current*` accessor.
- **`JsonWriter`** — streaming encoder. Owns a growable `int8[]`, tracks nesting
  depth and sibling state, so you never thread commas or colons yourself. Every
  `begin*`/`end*`/`write*`/`key` returns `this` for fluent chaining.

The reader and writer are independent — they cooperate only via bytes you carry
between them (see "Round-tripping raw bytes" below). There is no shared session
object.

## Reader: the next() pump

`next()` returns the next `JsonToken`, advancing the cursor over whitespace,
separators, and brackets; it returns `END` at top level when the buffer is
exhausted. Separator and key/colon handling are internal — inside an object the
reader emits a `KEY` token, then the following `next()` emits that key's value.

Construct with an explicit byte count (primitive arrays don't yet expose
`.count()` as a property everywhere, so pass it):

```cajeta
import cajeta.codec.json.JsonReader;
import cajeta.codec.json.JsonToken;
import cajeta.lang.String;

JsonReader r = heap JsonReader(buf, buf.count());   // buf is an int8[]
JsonToken t = r.next();
while (t != JsonToken.END) {
    if (t == JsonToken.KEY) {
        String key #= r.currentString();     // owned copy — see ownership
        JsonToken vt = r.next();             // this key's value
        if (vt == JsonToken.NUMBER) {
            int64 n = r.currentNumberAsInt64();
        } else if (vt == JsonToken.STRING) {
            String s #= r.currentString();
        }
    } else if (t == JsonToken.BOOLEAN) {
        boolean b = r.currentBoolean();      // BOOLEAN token carries no value itself
    }
    t = r.next();
}
```

`peek()` gives one-token lookahead (idempotent until the next `next()` consumes
it) — used to dispatch nested-class array elements. `skipValue()` consumes one
complete value (scalar or balanced nested subtree) without decoding it — the
unknown-key skip in the `Json.parse<T>` synthesizer.

### Span accessors (only valid for the current token)

- `currentNumberAsInt64()` / `currentNumberAsInt32()` — v1 parses **integers
  only**; a fractional/exponent form throws `JsonParseException`, as does
  out-of-`int64`-range. Use `currentNumberAsFloat64()` for decimals (naive, not
  strtod-accurate).
- `currentBoolean()` — true/false for the current `BOOLEAN` token.
- `currentString()` → `#String`, `currentBytes()` → `#int8[]`,
  `currentRawBytes()` → `#int8[]` (includes the surrounding quotes for
  STRING/KEY; identical to `currentBytes()` for NUMBER/BOOLEAN/NULL).

## Writer: the fluent builder

```cajeta
import cajeta.codec.json.JsonWriter;

JsonWriter w = heap JsonWriter();
w.beginObject()
 .key("name").writeString("Ada")
 .key("age").writeNumber((int64) 36)
 .key("admin").writeBoolean(true)
 .endObject();
#int8[] json = w.toBytes();   // {"name":"Ada","age":36,"admin":true}
```

`key(...)` emits `"name":` and suppresses the separator on the value that
follows; everything else (commas between siblings) is automatic. Scalars:
`writeNull` / `writeBoolean` / `writeNumber(int64)` / `writeNumber(float64)` /
`writeString(String)` (or the `int8[],int32` hot-path overload). `writeValue`
re-emits a `JsonValue` DOM tree. Output drains:

- `toBytes()` → `#int8[]` (fresh owned copy) **and resets the writer** for reuse.
- `writeTo(FileWriter sink)` — writes the internal buffer straight to the sink,
  no copy, then resets. `writeLineTo(sink)` appends a `\n` first (JSON Lines).

## Ownership and lifecycle (read before you cross a boundary)

- **Reader input is borrowed.** `JsonReader(input, byteCount)` stores the
  `int8[]` by reference and never frees it — the caller owns `buf` and must keep
  it alive for the reader's whole life. The reader internally heap-allocates its
  depth/state stacks (1024-deep); exceeding that depth throws.
- **Span accessors return owned copies.** `currentString()`/`currentBytes()`/
  `currentRawBytes()` each allocate a fresh `#`-owned result copied out of the
  input buffer — ownership transfers to you. They do **not** alias `buf`, so the
  value outlives the reader. (Cost: one allocation per call; on a true hot path
  read `tokenStart()/tokenEnd()` spans directly instead.)
- **`readValue()` returns an owned `#JsonValue` tree** — your drop chain reclaims
  it.
- **Writer chain returns borrowed `this`** (do not free the link results).
  `toBytes()` hands you an owned `#int8[]`. After `toBytes`/`writeTo`/
  `writeLineTo` the writer is **reset and reusable** for the next document.

## What this code does NOT do

- **String escapes are NOT decoded.** A STRING/KEY span and every `current*`
  byte/String accessor return the bytes **verbatim** — `\n` stays as the two
  bytes `\`,`n`, and surrounding logic must decode if it needs the literal char.
  The writer's `writeString` only escapes `"` and `\` (not control chars).
- **No floats in `currentNumberAsInt64`** (throws) and **no scientific notation**
  in `writeNumber(float64)` for very large/small magnitudes.
- **No relaxed JSON** — strict RFC 8259; comments and trailing commas throw.
- **`writeRaw` does not validate** — bytes must already be valid JSON; an invalid
  blob fails only on read-back.
- The reader does not give random access or back up — it is forward-only. For
  navigation, build the DOM.

## Round-tripping raw bytes (the reader↔writer handshake)

The one place the two classes are designed to cooperate: capture a value's exact
wire bytes from the reader and replay them through the writer byte-stably (the
`@JsonRaw` path). `currentRawBytes()` keeps the quotes for strings; `writeRaw`
emits them unchanged while still handling the surrounding separator.

```cajeta
// reader r is positioned on a primitive value token
#int8[] raw = r.currentRawBytes();
w.key("payload").writeRaw(raw, (int32) raw.count());
```

(Object/array values are not single-token span-capturable — v1 raw capture is
primitive values only.)

## Errors

Malformed input throws `JsonParseException` (a `RecoverableException`) carrying
the 0-based byte offset of the fault. After a throw the reader's state is
**undefined** — discard it and restart from a fresh source rather than calling
`next()` again.
