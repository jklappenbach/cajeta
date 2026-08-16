---
id: codec-json-Json
applies-to: [cajeta/codec/json/Json]
title: Json — top-level JSON parse/serialize entry points (three tiers)
description: Json static facade — Tier-3 JsonValue DOM (parse/toBytes) and Tier-1 reflection-synthesized typed parse<T>/toBytes<T> with field-naming annotations.
---

# Json — the JSON access point

`Json` is a `public final class` of only `static` methods (no instances — never
`heap Json()`). It is the **start-here** entry point for JSON in `cajeta.codec.json`.
Pick a tier by the routing table, then read this skill for the method's ownership and
the one big gotcha (Tier-1 is compiler-synthesized — its captured bodies *throw*).

| Task | Call | Returns | Notes |
|------|------|---------|-------|
| Known compile-time shape `T` → value | `Json.parse<T>(bytes, len)` / `Json.parse<T>(s)` | `T` | fastest; no DOM, no reflection |
| Value `T` → JSON bytes | `Json.toBytes<T>(value)` | `int8[]` | symmetric; see String caveat below |
| Unknown / dynamic shape → tree | `Json.parse(bytes, len)` / `Json.parse(s)` | `#JsonValue` | mutable DOM |
| `JsonValue` tree → JSON bytes | `Json.toBytes(value)` | `#int8[]` | symmetric with Tier-3 parse |
| Hot path / zero-alloc streaming | *not here* — use `JsonReader` / `JsonWriter` directly | — | Tier 2 |

`parse(...)` vs `parse<T>(...)` resolve by whether the call site supplies an explicit
type argument: bare `Json.parse(buf, n)` is Tier-3 (`JsonValue`); `Json.parse<T>(buf, n)`
is Tier-1 (synthesized `T`). Same disambiguation for `toBytes`.

## Signatures, ownership, errors

- `static #JsonValue parse(int8[] bytes, int64 length)` — borrows `bytes`; returns an
  **owned** (`#`) DOM the caller drops at scope. Throws `JsonParseException`
  (a `RecoverableException`) on malformed input.
- `static #JsonValue parse(String s)` — forwards to the above over `s.bytes` /
  `s.byteLength`; no copy. `s` must outlive use of the borrowed string views in the DOM.
- `static #int8[] toBytes(JsonValue value)` — borrows `value`; returns an **owned**
  byte buffer.
- `static T parse<T>(int8[] bytes, int64 length)` and `static T parse<T>(String s)` —
  Tier-1 typed parse. Returns a fresh `T` (class fields are owned by `T`).
- `static int8[] toBytes<T>(T value)` — Tier-1 typed serialize.
- `parseObjectFromReader<T>`, `walkValue<T>`, `walkElement<T>`, `toBytesObjectInto<T>` —
  internal recursion hooks the synthesizer emits for nested classes / array elements.
  Public only so cross-class codegen can call them; **do not call by hand**.

`length` is `int64`; pass `(int64) bytes.count()` (or `(int64) s.byteLength`). Missing
keys leave class fields default-initialized (class fields → `null`); unknown keys are
accept-and-skip unless the target class carries `@JsonStrict`.

## Tier-1 is synthesized at the call site — and that is the sharp edge

`parse<T>` / `toBytes<T>` (and the helpers above) have **captured bodies that throw**
`JsonParseException("...synthesizer not engaged...")`. They are failsafes: the real
per-field key-dispatch parse / sequential-write code is emitted by the compiler when it
walks `T`'s declared fields at the call site. If you ever see "synthesizer not engaged"
at runtime, the compile-side synthesizer didn't fire for that `T` — it is not a parse
error in your data. `T` must be a plain class with public fields (source-order walked);
no class-level annotation is required.

Supported field types (v1): `int32`, `int64`, `float64`, `boolean`, `String`, nested
class (recursive descent), and `T[]` arrays of primitive or class elements.
**Caveat (v1):** the `toBytes<T>` write path for `String` fields is not yet stable —
round-trip with String fields on the write side may fail; parse-side String is fine.
Use a primitives-only class for write round-trips (see example).

## Annotations (steer the field↔wire mapping)

Field-level: `@JsonProperty("wire")` (rename, read+write), `@JsonIgnore` (skip both),
`@JsonRequired` (throw `JsonParseException` if key missing), `@JsonAlias({"alt"})`
(extra accepted read keys), `@JsonInclude("NON_NULL")` (omit null on write).
Class-level: `@JsonNamingStrategy("SNAKE_CASE"|"KEBAB_CASE")`, `@JsonStrict` (throw on
unknown keys at read). `@JsonProperty` overrides naming strategy; `@JsonIgnore` wins
over `@JsonRequired`.

## Example — Tier-1 round-trip (mirrors the JSON tour)

```cajeta
import cajeta.codec.json.Json;

public class JsonNum {        // plain class, public fields, no annotations needed
    public int32 id;
    public int64 score;
    public boolean active;
}

JsonNum a = heap JsonNum();
a.id = 99;
a.score = 123456789L;
a.active = true;

int8[] bytes #= Json.toBytes<JsonNum>(a);             // {"id":99,...}
JsonNum b #= Json.parse<JsonNum>(bytes, (int64) bytes.count());
// b.id == 99, b.score == 123456789, b.active == true

// Parse straight from a String literal (no DOM alloc):
JsonNum c #= Json.parse<JsonNum>("{\"id\":7,\"score\":99,\"active\":false}");
```

## Example — Tier-3 dynamic DOM

```cajeta
import cajeta.codec.json.Json;
import cajeta.codec.json.JsonValue;

JsonValue v #= Json.parse("{\"id\":1,\"name\":\"alice\"}");   // owned DOM
int64 id = v.asObject().get("id").asInt64();                  // 1
String name #= v.asObject().get("name").asString();           // owned String view
// v drops at scope exit, freeing the whole tree.
```

DOM details (kinds, `asX()`/`setX()` builders, borrowed-vs-owned string slices) live in
the `JsonValue` / `JsonObject` / `JsonArray` class skills — not here. Note one carried
fact relevant to choosing a tier: DOM string views are **borrowed** over the parsed
bytes and escapes are kept verbatim; copy (`asString()`) to keep them.

## What Json does NOT do

- No streaming / zero-alloc surface here — that is `JsonReader.next()` /
  `JsonWriter` (Tier 2), used directly, not through `Json`.
- No float in the Tier-3 DOM (`JsonValue` NUMBER is `int64` in v1); Tier-1 has
  `float64`.
- Tier-1 `toBytes<T>` String-field write is not yet stable (v1).
- `Json` is never instantiated and holds no state — purely static.
