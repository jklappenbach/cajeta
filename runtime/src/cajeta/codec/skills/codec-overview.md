---
id: codec-overview
applies-to: [cajeta.codec]
title: cajeta.codec orientation — Base64, CSV, JSON routing
description: Pick the right codec entry point (Base64 / CSV / JSON) and learn the library-wide byte-array, ownership, and exception conventions.
---

# cajeta.codec — byte ⇄ text codecs

Encode and decode bytes to/from textual interchange formats: **Base64** (`cajeta.codec`),
**CSV** (`cajeta.codec.csv`), and **JSON** (`cajeta.codec.json`). The unit of currency
everywhere is a raw `int8[]` byte buffer plus an explicit `int64` length — never a stream
object, never a managed text reader. Every format exposes a **static facade** as its front
door; CSV and JSON additionally expose lower-level streaming classes for the hot path.

If your task is XML, YAML, TOML, MessagePack, protobuf, or URL/query encoding, it is **not
here** — this library is Base64 + CSV + JSON only.

## Task → entry point

| Want to…                                          | Start with                                                  |
|---------------------------------------------------|------------------------------------------------------------|
| Encode bytes to Base64 text                       | `Base64.encode(data, len)` / `Base64.encodeUrlSafe(...)`   |
| Decode Base64 text to bytes                       | `Base64.decode(String)` / `Base64.decodeBytes(data, len)`  |
| Parse JSON into a known class `T`                 | `Json.parse<T>(bytes, len)` (Tier 1 synthesizer)           |
| Parse JSON of unknown/ad-hoc shape               | `Json.parse(bytes, len)` → mutable `JsonValue` tree (Tier 3) |
| Serialize a class `T` to JSON bytes               | `Json.toBytes<T>(value)` (Tier 1)                          |
| Serialize a `JsonValue` tree to JSON bytes        | `Json.toBytes(value)` (Tier 3)                             |
| Hot-path JSON, zero tree allocation               | `JsonReader.next()` / `JsonWriter` (Tier 2)                |
| Write newline-delimited JSON (JSON Lines)         | `JsonLinesWriter`                                          |
| Parse CSV into a known class `T` / `T[]`           | `Csv.parse<T>(bytes, len)` (Tier 1 synthesizer)            |
| Stream CSV rows manually                          | `CsvReader.nextRow()` / `field(i)`                         |
| Write CSV                                         | `CsvWriter.writeField()` / `endRow()` / `toBytes()`        |

**Negative rows (don't go hunting):**
- No streaming/incremental Base64 — `Base64` is one-shot over a whole buffer only.
- Tier-3 `JsonValue` NUMBER is **int64-only** in v1 (`asInt64()`/`asInt32()`); there is no
  float payload on the tree. Floats survive only through Tier-1/Tier-2 (`JsonWriter.writeNumber(float64)`,
  `JsonReader.currentNumberAsFloat64()`).
- No JSON Schema / validation layer; no CSV type sniffing beyond the declared `T` fields
  (`int32`/`int64`/`float64`/`boolean`/`String`).
- CSV `Csv.parse<T>` needs a **header row** to bind columns to fields.

## Library-wide conventions (learn once, applies everywhere)

- **Byte buffer + explicit length.** Entry points take `(int8[] bytes, int64 length)`
  because cajeta primitive arrays don't yet expose `.count()` as a property accessor; pass
  `(buf, (int64) buf.count())`. `String` overloads exist on every facade and forward to the
  byte form using the String's UTF-8 payload directly (no copy).
- **Ownership is marked with `#`, and the `#` that transfers is the one at the CALL SITE.**
  A `#` on a **return type** means *the caller owns* the fresh buffer/object (it drops at
  the caller's scope). A `#` on a **parameter** does not itself transfer: it *obliges* you
  to surrender title by writing `#arg` at the call site (a plain `arg` is
  `CAJETA_ERROR_TRANSFER_REQUIRED`). A plain `T` parameter accepts either spelling —
  `f(x)` lends, `f(#x)` transfers — so the call-site `#` is always what moves the title,
  and only then does the caller's local deactivate. A bare (no-`#`) return is a
  **borrowed view** the caller must not free and must not outlive the source — e.g.
  `JsonValue.asBytes()` and `JsonObject.get(...)` hand back borrows into the `JsonValue`
  tree (copy to keep them past the tree's lifetime).
- **Errors are exceptions, not sentinels.** All three formats throw a
  `cajeta.error.RecoverableException` subtype on malformed input — `Base64Exception`,
  `CsvParseException`, `JsonParseException` — each carrying an `int64 position` byte offset.
  Recoverable (not Unrecoverable) because decode boundaries routinely see untrusted input,
  so catch or declare them. Lookups that can legitimately miss return `null`
  (`JsonObject.get`) or `Optional.empty()` (`JsonObject.getString`/`getInt`/…) instead of
  throwing.
- **No drop-on-scope close needed.** Facades are stateless statics; `CsvReader`/`CsvWriter`/
  `JsonReader`/`JsonWriter` are plain heap/stack objects with no `close()` — they drop with
  their scope. (`JsonLinesWriter`/`JsonWriter.writeTo` write into a caller-supplied
  `FileWriter`; that sink's lifecycle is the caller's.)
- **Tier 1 is a compile-time synthesizer.** `Json.parse<T>` / `Json.toBytes<T>` /
  `Csv.parse<T>` are rewritten per-`T` by the compiler (field-walk codegen, no reflection,
  no value tree). The facade body you see in source is a **failsafe** that throws the
  format's parse exception if the synthesizer fails to engage — so a thrown
  "synthesizer not engaged" means a toolchain problem, not bad data.

## Canonical end-to-end example (JSON round-trip, Tier 1)

```cajeta
import cajeta.codec.json.Json;

public class User {
    public int32 id;
    public String name;
}

User u = heap User();
u.id = 7;
u.name = "alice";

int8[] bytes #= Json.toBytes<User>(u);                  // {"id":7,"name":"alice"}, caller-owned
User v #= Json.parse<User>(bytes, (int64) bytes.count());
// or straight from text:
User w #= Json.parse<User>("{\"id\":7,\"name\":\"alice\"}");
```

Base64 in two lines:

```cajeta
import cajeta.codec.Base64;

String text #= Base64.encode(raw, (int64) raw.count());   // standard, padded; caller owns
int8[] back #= Base64.decode(text);                       // throws Base64Exception on garbage
```

## Disambiguation — which JSON tier

- **Tier 1** (`Json.parse<T>`/`toBytes<T>`, `Csv.parse<T>`): known compile-time shape, want
  zero intermediate allocation and direct field binding. Default choice for typed data.
- **Tier 3** (`Json.parse` → `JsonValue`): shape unknown at compile time, or you want to
  inspect/mutate an ad-hoc tree. Convenient, ~48 bytes/node — not the hot path.
- **Tier 2** (`JsonReader`/`JsonWriter`): you control every allocation and pull tokens
  yourself. Use only when Tier 1/3 allocation shows up in a profile.

For CSV: `Csv.parse<T>` is the typed facade; drop to `CsvReader`/`CsvWriter` for untyped or
streaming row work.

## Hazards

- **JSON string escapes are kept verbatim.** `JsonValue.asBytes()`/`asString()` and
  `JsonReader.currentBytes()`/`currentString()` return the raw on-wire bytes including `\n`,
  `\uXXXX` etc. — they are **not** unescaped in v1. Decode yourself if you need the literal.
- **`JsonObject.put` does not deduplicate** — re-putting a key appends a shadow entry `get`
  never reaches.
- **`CsvReader` field decoders are permissive in v1** — `parseI64`/`parseF64` skip non-digit
  bytes rather than rejecting; don't rely on them to validate.
- **`CsvWriter` leaks its old buffer on grow** (known gap); fine for bounded output.

## Setup

Pure cajeta — no `@Native` bridge (Base64 is index arithmetic). Imports:
`cajeta.codec.Base64`, `cajeta.codec.csv.*`, `cajeta.codec.json.*`. Tier-1 typed parse/emit
require the compiler synthesizer (Phase 4b, `MethodTemplateInstantiator`).

## Go deeper

- Package: `cajeta/codec/json`, `cajeta/codec/csv`.
- Classes: `cajeta/codec/json/JsonReader`, `JsonWriter`, `JsonValue`/`JsonObject`/`JsonArray`
  (Tier-3 DOM component); `cajeta/codec/csv/CsvReader`, `CsvWriter`.
