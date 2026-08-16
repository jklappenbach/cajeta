---
id: codec-json-overview
applies-to: [cajeta/codec/json]
title: JSON codec neighborhood — facade, DOM, streaming, SAX
description: Route a JSON task to Json facade vs JsonValue DOM vs JsonReader/JsonWriter streaming vs JsonSax, with ownership rules.
---

# cajeta.codec.json — the JSON neighborhood

This package reads and writes RFC 8259 JSON over UTF-8 `int8[]` buffers. Pick the
access layer by your task; do not read all four.

## Which layer — decision table

| Task | Use | Returns / takes |
|------|-----|-----------------|
| Known compile-time shape `T`, fastest | `Json.parse<T>` / `Json.toBytes<T>` | `T` ↔ `#int8[]` (no tree) |
| Unknown/dynamic shape, inspect or mutate | `Json.parse` → `JsonValue` DOM | `#JsonValue` tree |
| Hot path, full allocation control, one forward pass | `JsonReader` (pull) / `JsonWriter` (push) | tokens / fluent emit |
| Event callbacks while streaming once | `JsonSax.parse` + a `JsonHandler` subclass | void (push events) |
| One value per line (`.jsonl`) to a file | `JsonLinesWriter` | streamed records |

Negative routing — what is **not** here:
- No float in the DOM: `JsonValue` NUMBER is `int64` only (`asInt64`/`asInt32`); floats
  survive only through `JsonReader.currentNumberAsFloat64` / `JsonWriter.writeNumber(float64)`.
- String escapes are **never decoded** anywhere — `\n` stays the two bytes `\ n` in
  `currentBytes`, `JsonValue` strings, and DOM round-trips. There is no unescape helper.
- No hashing in `JsonObject.get` (linear scan); no key dedup in `put`.
- No relaxed JSON (comments, trailing commas) — strict RFC 8259.
- `Json.parse<T>` is compiler-synthesized; the runtime bodies just throw
  `JsonParseException("...synthesizer not engaged")` as a failsafe.

## Inventory

Entry points (instantiate / call):
- `Json` — static facade; the three-tier entry table above. Start here.
- `JsonReader` — pull tokenizer (Tier 2 read).
- `JsonWriter` — streaming encoder (Tier 2 write).
- `JsonSax` — static push driver over `JsonReader`.
- `JsonLinesWriter` — JSONL record writer wrapping a `JsonWriter` + `FileWriter`.

DOM component (Tier 3 value tree): `JsonValue` (tagged union) + `JsonObject`
(insertion-ordered key/value) + `JsonArray` (growable element list).

Support types: `JsonToken` (enum: `START_OBJECT`/`KEY`/`STRING`/`NUMBER`/`BOOLEAN`/
`NULL`/`END`/…), `JsonHandler` (override-the-events base class for SAX),
`JsonParseException` (a `RecoverableException` carrying a byte `position`).

Internal synthesis substrate — you do **not** call these by hand; the `Json.parse<T>`
codegen drives them: `JsonIndex` (SIMD structural index, `build`), `JsonCursor` (walk
state passed to synthesized `Json.walkValue<T>`). Reading them only matters when
debugging Tier-1 codegen.

## Collaboration

- `Json.parse(bytes,len)` builds a `JsonReader`, calls `readValue()`, returns a
  `#JsonValue`. `Json.toBytes(value)` builds a `JsonWriter`, calls `writeValue` +
  `toBytes()`.
- `JsonReader.readValue()` recursively materializes `JsonObject`/`JsonArray`/`JsonValue`
  (the DOM). `JsonWriter.writeValue(JsonValue)` walks the DOM back to bytes.
- `JsonSax.parse` loops `JsonReader.next()` and dispatches to the matching
  `JsonHandler.on*`; value events hand you the live `JsonReader` so you pull only what
  you want (`r.currentString()`, `r.currentNumberAsInt64()`). `onBoolean(boolean)` and
  `onNull()` carry no reader.

## Ownership / lifecycle (package-wide)

- `#` on a return = owned, caller's drop chain frees it: `Json.parse` → `#JsonValue`,
  `Json.toBytes`/`JsonWriter.toBytes` → `#int8[]`, `JsonReader.currentBytes`/
  `currentString` → fresh owned copy.
- `#` on a param = transfer **required** in. `JsonObject.put(#int8[] key, len, #JsonValue)`,
  `JsonArray.add(#JsonValue)`, `JsonValue.setArray/setObject(#…)` and
  `JsonValue.setStringOwned(#int8[],len)` oblige you to surrender title at the call
  site — `obj.put(#key, len, #val)`, `arr.add(#v)`, `v.setArray(#a)`, `v.setObject(#o)`,
  `v.setStringOwned(#bytes, len)`. The plain spelling (`arr.add(v)`) is
  `CAJETA_ERROR_TRANSFER_REQUIRED`. The call-site `#` is what transfers, and only then
  does the caller's local deactivate.
- Borrowed (do NOT free, copy to keep beyond the source's lifetime): `JsonValue.asBytes()`
  and the bytes behind `JsonValue.setString(bytes,len)` are views; `JsonObject.get(...)`
  and `JsonArray.get(i)` return borrowed elements owned by the container; the input
  `int8[]` you pass to a reader is borrowed (not freed by the reader).
- `JsonReader` / `JsonWriter` need no `close()` — drop reclaims them. `JsonWriter.toBytes`,
  `writeTo`, `writeLineTo` each **reset** the writer for reuse. After a thrown
  `JsonParseException` the reader state is undefined — discard it.
- Errors: malformed input throws `JsonParseException(message, position)`; `JsonObject`
  typed getters return `Optional.empty()` on missing-key or kind-mismatch, while
  `get(key)` returns `null` on miss.

## Worked examples (with imports)

DOM — dynamic shape:
```cajeta
import cajeta.codec.json.Json;
import cajeta.codec.json.JsonValue;
import cajeta.lang.String;

JsonValue v #= Json.parse("{\"id\":1,\"name\":\"alice\"}");
int64 id = v.asObject().get("id").asInt64();       // 1
String name = v.asObject().getString("name").get();
```

SAX — count keys in one streaming pass:
```cajeta
import cajeta.codec.json.JsonSax;
import cajeta.codec.json.JsonHandler;
import cajeta.codec.json.JsonReader;

public class Counter extends JsonHandler {
    public int64 keys;
    public Counter() { this.keys = 0; }
    public void onKey(JsonReader r) { this.keys = this.keys + 1; }
}
Counter c = heap Counter();
JsonSax.parse(bytes, length, c);                   // c.keys == object-key count
```

Tier-1 round-trip (compiler-synthesized, see field/annotation tables in the `Json`
class docs):
```cajeta
import cajeta.codec.json.Json;

int8[] bytes #= Json.toBytes<User>(u);              // {"id":7,"name":"alice"}
User v #= Json.parse<User>(bytes, (int64) bytes.count());
```

## Deeper

Per-class detail (signatures, every accessor) lives in the class-level docs of `Json`,
`JsonReader`, `JsonWriter`, `JsonValue`/`JsonObject`/`JsonArray`, and `JsonSax`/
`JsonHandler`. Tier-1 design notes: `docs/specification/codec/json/Json.md`,
`plans/Json-fast-path.md`.
