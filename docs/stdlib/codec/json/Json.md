# Json

`cajeta.codec.json.Json` — the top-level JSON entry points, in three tiers.
Tier 1 (`Json.parse<T>` / `Json.toBytes<T>`) is the synthesizer: the compiler
walks `T`'s declared fields at the call site and emits per-field parse/write
code directly — no `JsonValue` tree, no runtime reflection. Tier 3
(`Json.parse` / `Json.toBytes` without type arguments) parses into a mutable
`JsonValue` tree, for shapes not known at compile time. Tier 2 is the raw
`JsonReader` / `JsonWriter` pull tokenizer, for hot paths where allocation
matters. Field-level annotations (`@JsonProperty`, `@JsonIgnore`,
`@JsonRequired`, `@JsonAlias`, `@JsonInclude`) and class-level ones
(`@JsonNamingStrategy`, `@JsonStrict`) steer the Tier-1 mapping when defaults
aren't enough.

```cajeta
import cajeta.codec.json.Json;

public class User {
    public int32 id;
    public boolean active;
}

public class JsonExample {
    public void run() {
        User u = heap User();
        u.id = 7;
        u.active = true;
        int8[] bytes #= Json.toBytes<User>(u);                     // {"id":7,"active":true}
        User v #= Json.parse<User>(bytes, (int64) bytes.count());
        User w #= Json.parse<User>("{\"id\":7,\"active\":true}");
    }
}
```

## Methods

| Signature | |
|---|---|
| `static #JsonValue parse(int8[] bytes, int64 length)` ⚑ | Tier 3: parse a JSON byte buffer into a mutable `JsonValue` tree; throws `JsonParseException` on malformed input |
| `static #JsonValue parse(String s)` ⚑ | Tier-3 `String` overload — parses the `String`'s UTF-8 payload directly, no copy |
| `static #int8[] toBytes(JsonValue value)` ⚑ | Tier 3: serialize a `JsonValue` tree back to a fresh JSON byte buffer |

Tier-3 builder ownership: `JsonValue.setString(String)` COPIES its argument
(the value is self-contained; the source may drop freely). The aliasing
variant is `setStringBorrowed(s)` — sharp, source must outlive the value —
and `setStringOwned(#bytes, len)` transfers a byte buffer in.
`array()` / `object()` are VIEWS of the held nodes (borrows, valid while
the `JsonValue` lives), and `asString()` is a producer returning an owned
copy.
| `static T parse<T>(int8[] bytes, int64 length)` ⚑ | Tier 1: parse JSON bytes into a `T` by synthesizing per-`T` parse code at the call site |
| `static T parse<T>(String s)` ⚑ | Tier-1 `String` overload — forwards to the byte-buffer variant |
| `static int8[] toBytes<T>(T value)` ⚑ | Tier 1: emit `T` to JSON bytes; symmetric counterpart of `parse<T>` |
| `static T parseObjectFromReader<T>(JsonReader r)` | Tier-1 helper: read a `T` from an existing `JsonReader` position (used by synthesized nested-class recursion) |
| `static T walkValue<T>(JsonCursor jc)` | Tier-1 binding walk: consume one object of type `T` at the cursor over the SIMD structural index and advance past it |
| `static T walkElement<T>(JsonCursor jc)` | Delegating wrapper over `walkValue<T>` for nested-class array elements |
| `static void toBytesObjectInto<T>(JsonWriter w, T value)` | Tier-1 helper: emit `T` into an existing `JsonWriter` (nested-class recursion) |

⚑ = `@EntryPoint`

The `parseObjectFromReader` / `walkValue` / `walkElement` / `toBytesObjectInto`
helpers are public so the synthesizer can emit cross-class recursive calls;
application code normally uses only `parse` / `toBytes`.

## See also

- Tour: [JsonDemo](../../../../samples/tour/src/main/cajeta/tour/codec/JsonDemo.cajeta)
- [Csv](../csv/Csv.md) — the CSV Tier-1 synthesizer built on the same pattern
- [Base64](../Base64.md) — the other `cajeta.codec` codec
- Source: [`runtime/src/cajeta/codec/json/Json.cajeta`](../../../../runtime/src/cajeta/codec/json/Json.cajeta)
