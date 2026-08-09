---
id: codec-json-dom
applies-to: [cajeta/codec/json/JsonValue, cajeta/codec/json/JsonArray, cajeta/codec/json/JsonObject]
title: JSON DOM (Tier 3) — JsonValue tree, JsonObject, JsonArray
description: In-memory JSON tagged-union tree — kind-gated reads, Optional getters, and #-ownership of nodes and borrowed string bytes.
---

# JSON DOM — the Tier 3 value tree

Use this trio when the JSON shape is **not known at compile time** and you want to
inspect or mutate an ad-hoc tree. For a known struct prefer Tier 1
(`Json.parse<T>` / `Json.toBytes<T>`); for streaming with zero allocation prefer
Tier 2 (`JsonReader` / `JsonWriter`). This DOM is the slowest, allocation-heaviest
tier — a `JsonValue` is ~48 bytes regardless of kind.

## Members and roles
- **`JsonValue`** — a tagged union. `kind()` returns one of the int32 constants
  `JsonValue.NULL`(0) `BOOLEAN`(1) `NUMBER`(2) `STRING`(3) `ARRAY`(4) `OBJECT`(5).
  The payload field matching `kind` is meaningful; the rest are zero/null. It is
  the only node type you hold by reference — `JsonObject`/`JsonArray` always live
  inside a `JsonValue`.
- **`JsonObject`** — insertion-ordered key/value store backing `kind OBJECT`.
  Linear-scan lookup (no hashing in v1).
- **`JsonArray`** — geometric-growth ordered list backing `kind ARRAY`.

## Object graph & ownership (read this first)
- A `JsonValue` of kind OBJECT/ARRAY **owns** its `JsonObject`/`JsonArray`, which
  in turn own their child `#JsonValue`s. The whole tree is reclaimed by dropping
  the root — usually the `#JsonValue` returned from `Json.parse(...)`.
- Putting/adding into the tree **requires you to transfer ownership**:
  `JsonArray.add(#JsonValue)`, `JsonObject.put(#int8[] key, int32 keyLen, #JsonValue)`,
  `JsonValue.setArray(#JsonArray)`, `setObject(#JsonObject)`,
  `setStringOwned(#int8[], len)`. A `#` formal never transfers on its own — it obliges
  you to write `#` at the **call site** (`arr.add(#v)`; a plain `arr.add(v)` is
  `CAJETA_ERROR_TRANSFER_REQUIRED`). Only after that does the caller's local deactivate.
- **Borrowed (do NOT free, copy to keep beyond the tree's life):** `asBytes()`
  returns the node's `int8[]` view; `JsonObject.get(...)`, `valueAt`, `JsonArray.get`
  return **borrowed `JsonValue`** references still owned by the tree.
- `asString()` returns an **owned `#String`** (a fresh view-mode String over the
  borrowed bytes) — or **null for non-STRING kinds**, so gate on `kind()`/check null.
- `setString(bytes, len)` and `setString(String)` only **borrow** the bytes — the
  source must outlive the JsonValue. Use `setStringOwned(#int8[], len)` to transfer.

## Reading a parsed tree (kind-gate, then the typed getters)
```cajeta
import cajeta.codec.json.Json;
import cajeta.codec.json.JsonValue;
import cajeta.codec.json.JsonObject;
import cajeta.lang.Optional;
import cajeta.lang.String;

JsonValue root = Json.parse("{\"name\":\"alice\",\"tags\":[\"a\",\"b\"]}");   // owned — the factory returns `#JsonValue`
if (root.kind() == JsonValue.OBJECT) {
    JsonObject obj = root.asObject();          // borrowed, owned by root

    // Typed getters return Optional.empty() on missing key OR kind mismatch
    Optional<String> name = obj.getString("name");
    if (name.isPresent()) { String n = name.get(); }

    // Raw get() returns the borrowed JsonValue, or null on miss
    JsonValue tags = obj.get("tags");
    if (tags != null && tags.kind() == JsonValue.ARRAY) {
        JsonArray a = tags.asArray();
        int32 i = 0;
        while (i < a.count()) {
            String s = a.get(i).asString();   // owned; null if elem not STRING
            i = i + 1;
        }
    }
}
// dropping `root` reclaims the whole tree (object, array, and every child)
```

`JsonObject` typed getters: `getString`/`getInt`(int32)/`getLong`(int64)/`getBoolean`/
`getArray`/`getObject` → `Optional<…>`, empty when the key is absent **or** the value's
kind doesn't match (a malformed `{"min":"eighty"}` yields empty, never a silent
default). Also: `containsKey(String)`, `keys()` (`#ArrayList<String>`, one String per
key), `getStringArray(String)` (`#ArrayList<String>`, empty when absent/non-array,
skips non-string elements). For the hot path use the byte-buffer forms
`get(int8[], len)` / `keyAt` / `keyLenAt` / `valueAt` / `count` instead.

## Building a tree by hand
```cajeta
import cajeta.codec.json.JsonValue;
import cajeta.codec.json.JsonArray;

JsonArray arr = heap JsonArray();
arr.add(#heap JsonValue().setNumber(1));        // setX mutate `this` and chain;
arr.add(#heap JsonValue().setBoolean(true));    // `#` prefixes the whole argument
JsonValue v = heap JsonValue().setArray(#arr);  // the call-site `#` is what transfers
```

## What this DOM does NOT do
- No float/double — `NUMBER` is `int64` only in v1 (`asInt32` truncates).
- `JsonObject.put` does **not** deduplicate: re-putting a key appends a second
  entry that `get` (first-match scan) will never reach.
- No drop-on-scope magic beyond ownership — a node you build but never `add`/`put`
  into the tree is owned by its local and dropped at scope exit; once transferred
  (`#`) the local is dead, so don't touch it.
- String bytes are kept **verbatim** (escapes not decoded) — same as the reader.
- This is not the parser/serializer: `JsonReader.readValue()` / `Json.parse`
  produce the tree, `Json.toBytes(JsonValue)` writes it back.
