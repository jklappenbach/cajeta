---
id: codec-json-sax
applies-to: [cajeta/codec/json/JsonSax, cajeta/codec/json/JsonHandler]
title: Event-driven (SAX) JSON parsing with JsonSax + JsonHandler
description: Subclass JsonHandler, override the events you want, drive once with JsonSax.parse; pull values lazily from the JsonReader the events hand you.
---

# JSON SAX: JsonSax + JsonHandler

**To consume a JSON document as a stream of events** (no tree built), subclass
`JsonHandler`, override only the events you need, and call
`JsonSax.parse(bytes, byteCount, handler)`. The parser walks the document once,
in document order, calling your handler. Memory is O(nesting depth), independent
of document size — nothing is buffered.

This is the *push* API. For *pull* control (drive `next()` yourself, skip
subtrees), use `JsonReader` directly. For a materialized tree, use `Json.parse`.

## Members and roles

- **`JsonSax`** — `final` class with one static method, `parse`. The driver. You
  never instantiate it. Internally it allocates a `JsonReader` over your bytes and
  loops `next()`, dispatching each `JsonToken` to a handler method.
- **`JsonHandler`** — the base you subclass. Every method is a no-op by default,
  so override only what you care about. Events:
  `onStartObject()`, `onEndObject()`, `onStartArray()`, `onEndArray()`,
  `onKey(JsonReader r)`, `onString(JsonReader r)`, `onNumber(JsonReader r)`,
  `onBoolean(boolean v)`, `onNull()`.
- **`JsonReader`** (collaborator, see its own skill) — the value events receive it
  positioned at the current token so you can pull the value lazily.

## Collaboration / object graph

`JsonSax.parse` creates a `JsonReader` from your `int8[]` and owns the loop. For
each token it either calls a structural event (objects/arrays carry no data) or a
value event. Value events hand you **the same reader instance**, already
positioned, and you pull what you want from it. `onBoolean` is passed the decoded
`boolean` directly (the `BOOLEAN` token carries no span); `onNull` takes no
argument. The handler is borrowed for the duration of the call — `JsonSax` does
not retain or free it; you own its lifetime.

## Pulling values (lazy)

Values are **not decoded unless you ask**. In a value event, pull from `r`:

- `r.currentString()` → `#String` — the string/key text. **Owned, transferred to
  you.** Its bytes are a fresh copy of the token span, so it stays valid after the
  parse ends — keep it freely.
- `r.currentNumberAsInt64()` / `currentNumberAsInt32()` / `currentNumberAsFloat64()`
  → the number. v1 int parsing is signed-integer only; a fractional/exponent token
  fed to `currentNumberAsInt64()` throws `JsonParseException`. Use the float64
  variant for those.
- The `boolean` comes to `onBoolean` as the argument — do not call the reader for it.

If an event ignores its value, that value costs nothing to decode.

## Ownership across the component boundary

- **Input `int8[]` is borrowed by the parse, not freed.** `JsonReader` stores a
  reference to your buffer and reads spans out of it; `JsonSax.parse` returns
  without freeing it. The buffer **must outlive the parse call**. You free it.
- **The `JsonReader` handed to events is borrowed — do not retain it.** It is the
  driver's reader, repositioned on every token. Capturing it for later use is a
  use-after-position bug. Extract what you need (e.g. `currentString()`, which
  copies) during the call.
- `currentString()`'s `#String` return is the one thing you own and may keep.

## What this does NOT do

- No tree, no DOM — you get events, not a `JsonValue`. (Want a tree? `Json.parse`.)
- No random access / no field lookup — events arrive in document order only. To
  jump to a few fields and skip the rest, drive `JsonReader` yourself and use its
  `skipValue()` / structural index.
- No `byteCount` inference — you pass the length explicitly (e.g. `b.count()`).
- No restart after an error: on malformed input a value/`next()` step throws
  `JsonParseException` (a `RecoverableException` carrying a 0-based byte
  `position`); the reader's state is then undefined. Catch it at the `parse` call
  and start over from a fresh source — do not resume.

## Worked example

```cajeta
import cajeta.io.file.File;
import cajeta.codec.json.JsonReader;
import cajeta.codec.json.JsonHandler;
import cajeta.codec.json.JsonSax;

// Override only the events of interest; the rest stay no-ops.
class SumHandler extends JsonHandler {
    public int64 keys;
    public int64 numbers;
    public SumHandler() { this.keys = 0; this.numbers = 0; }
    public void onKey(JsonReader r) {
        #String k = r.currentString();   // owned copy; safe to keep
        this.keys = this.keys + 1;
    }
    public void onNumber(JsonReader r) {
        int64 n = r.currentNumberAsInt64();
        this.numbers = this.numbers + n;
    }
}

int8[] b = File.readAllBytes("/tmp/data.json");  // caller owns b
int64 n = (int64) b.count();
SumHandler h = heap SumHandler();
JsonSax.parse(b, n, h);                           // b borrowed, not freed
// h.keys / h.numbers now populated
```

## When to use

- **SAX (here):** scan the whole document once, react to events, low memory — e.g.
  counting, summing, streaming a large file.
- **Pull (`JsonReader`):** you want to read a few fields and `skipValue()` the rest,
  or need lookahead (`peek()`).
- **Tree (`Json.parse` → `JsonValue`):** you need random access / a navigable
  in-memory structure.
