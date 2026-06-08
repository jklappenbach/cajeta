# cajeta.codec.json

JSON value model, parser, and writer for the cajeta stdlib. Single
source of truth for what "JSON support" means in Cajeta — consumed by
`@ToString(format=TO_STRING_JSON)` (Annotations.md § @ToString) and
direct `Json.parse<T>(bytes)` / `Json.toBytes(value)` calls in user
code. Mapping between JSON keys and class fields is driven by per-
field annotations (`@JsonProperty`, `@JsonIgnore`, `@JsonRequired`,
`@JsonNamingStrategy` at class level); no class-level annotation is
required to make a type JSON-compatible.

Status: **designed, implementation pending** (Features.md S-1101 ✅,
S-1102 ⏳).

## Goals, in priority order

1. **Fast.** Throughput is the design's first constraint, not an
   afterthought. The reader streams tokens without materializing a
   value tree; the writer formats primitives directly into a growable
   byte buffer; the `@Encoding` codegen path bypasses both layers and
   talks field-by-field to the user's struct. The library must be
   competitive with hand-tuned scalar JSON parsers in C and Java (rough
   target: ≥ 500 MB/s for the pull tokenizer on simple shapes on a
   modern x86_64 core); SIMD-accelerated structural scanning is a
   future direction, not a v1 dependency.
2. **RFC 8259 conformance.** Strict by default — invalid input fails
   loudly. A `relaxed=true` reader option allows trailing commas and
   `//` / `/* */` comments for hand-written configs.
3. **Three API tiers.** Direct codegen for `@Encoding`, pull tokenizer
   for streaming/zero-allocation, value tree for convenience. Users
   pick the layer that matches their performance budget.
4. **Zero-copy where the borrow checker allows.** Reader tokens carry
   borrowed slices into the input buffer; numbers are stored as
   unparsed byte slices and lazily decoded when accessed. The caller
   pays for materialization only on demand.

---

## Three API tiers

```
┌─────────────────────────────────────────────────────────────┐
│  Tier 3 — Value tree (JsonValue, JsonObject, JsonArray)     │  slowest, easiest
│  Allocates everything; HashMap-backed objects; recursive    │
│  walk; null-tolerant accessors.                             │
└─────────────────────────────────────────────────────────────┘
         ▲                                       │
         │ JsonReader.readValue()                │ JsonWriter.writeValue()
         │                                       ▼
┌─────────────────────────────────────────────────────────────┐
│  Tier 2 — Pull tokenizer / token writer                     │  fast, manual
│  JsonReader.next() returns JsonToken; user dispatches.      │
│  Strings/numbers are borrowed byte slices, parsed on demand.│
│  JsonWriter exposes beginObject/key/value/endObject calls.  │
└─────────────────────────────────────────────────────────────┘
         ▲                                       │
         │ compile-time codegen per T            │ compile-time codegen per T
         │                                       ▼
┌─────────────────────────────────────────────────────────────┐
│  Tier 1 — Json.parse<T>(bytes) / Json.toBytes(value)        │  fastest, zero-config
│  Method-level templates; the compiler walks T's declared    │
│  fields and emits per-field reader / writer code that talks │
│  directly to JsonReader / JsonWriter. No JsonValue tree.    │
│  Per-field annotations (@JsonProperty, @JsonIgnore, etc.)   │
│  drive the mapping; no class-level annotation required.     │
└─────────────────────────────────────────────────────────────┘
```

A user writing a server hot-path calls `Json.parse<T>(bytes)` —
nothing else required; the compiler does the work. A user writing a
SAX-style streaming consumer drops to Tier 2. A user writing ad-hoc
config inspection picks Tier 3 and pays for the convenience.

---

## Value model — `JsonValue`

```cajeta
package cajeta.codec.json;

public enum JsonKind {
    NULL, BOOLEAN, NUMBER, STRING, ARRAY, OBJECT
}

public class JsonValue {
    public JsonKind kind();

    // Type-narrowed accessors. Each throws JsonTypeException if kind
    // doesn't match — the catch-or-let-it-fly choice belongs to the
    // user, not the library. No silent coercion.
    public boolean asBoolean();      // BOOLEAN
    public JsonNumber asNumber();    // NUMBER
    public String asString();        // STRING
    public JsonArray asArray();      // ARRAY
    public JsonObject asObject();    // OBJECT
    public boolean isNull();         // NULL test (no exception)

    // Convenience builders. Each is the same as `heap JsonValue(...)`.
    public static #JsonValue ofNull();
    public static #JsonValue ofBoolean(boolean v);
    public static #JsonValue ofNumber(int64 v);
    public static #JsonValue ofNumber(float64 v);
    public static #JsonValue ofString(#String v);
    public static #JsonValue ofArray(#JsonArray v);
    public static #JsonValue ofObject(#JsonObject v);
}
```

### Why an enum-tagged ADT and not class inheritance

Class inheritance (`JsonNull extends JsonValue`, `JsonString extends
JsonValue`, etc.) would require a vtable dispatch on every kind-check
and a per-value heap allocation. The tagged-enum form lets the
compiler emit a `switch(kind)` for the hot paths and keeps the value
layout flat — `JsonValue` is the same fixed-size class regardless of
contents (with a `union`-style payload field).

### `JsonNumber` — lazy parse

```cajeta
public class JsonNumber {
    // The unparsed token bytes (borrowed into the input buffer for
    // reader-produced values; owned String for builder-produced).
    public boolean isInteger();          // no '.' / 'e' / 'E' in bytes
    public boolean fitsInt32();          // checks magnitude after parse
    public boolean fitsInt64();
    public int32 asInt32();              // parses, throws on overflow
    public int64 asInt64();
    public float64 asFloat64();
    public String raw();                 // the original byte slice as text
}
```

Numbers are kept as raw byte slices until the caller asks for a
concrete numeric type. A reader that only walks the structure of a
document (e.g., to extract one specific field) pays *zero* number-
parsing cost for the values it doesn't touch.

### `JsonObject` and `JsonArray`

```cajeta
public class JsonArray extends Stream<JsonValue> {
    public int64 size();
    public JsonValue get(int64 i);             // throws on out-of-bounds
    public Optional<JsonValue> getOpt(int64 i);// null-tolerant
    public void add(#JsonValue v);             // mutating builder
    public #Optional<JsonValue> next();        // Stream protocol
}

public class JsonObject extends Stream<JsonEntry> {
    public int64 size();
    public JsonValue get(String key);          // throws if absent
    public Optional<JsonValue> getOpt(String key);
    public boolean containsKey(String key);
    public void put(String key, #JsonValue v); // mutating builder
    public #Optional<JsonEntry> next();        // Stream<JsonEntry>
}

public class JsonEntry {
    public String key();
    public JsonValue value();
}
```

`JsonArray` is a contiguous `ArrayList<JsonValue>`. `JsonObject` is
order-preserving by default (insertion order). Both extend
`Stream<...>` so the standard combinators (`filter`, `map`, `forEach`)
apply uniformly.

---

## Tier 2 — pull tokenizer

```cajeta
public enum JsonToken {
    START_OBJECT,       // {
    END_OBJECT,         // }
    START_ARRAY,        // [
    END_ARRAY,          // ]
    KEY,                // (object key, before its value)
    STRING,             // quoted string value
    NUMBER,             // numeric literal
    BOOLEAN,            // true / false
    NULL,               // null
    END                 // input exhausted
}

public class JsonReader {
    public JsonReader(byte[] input);
    public JsonReader(byte[] input, JsonReaderOptions opts);

    public JsonToken next();              // advance to next token
    public JsonToken current();           // last token returned

    // Borrowed accessors — valid only until the next next() call,
    // since the underlying byte slice can be re-read or invalidated.
    // The borrow checker enforces this via the standard borrow-from-
    // receiver rule (Stream.findFirst etc. already do the same).
    public byte[] currentBytes();         // raw byte slice of current token
    public String currentString();        // STRING: decoded + unescaped
    public String currentKey();           // KEY: decoded + unescaped
    public JsonNumber currentNumber();    // NUMBER: lazy wrapper

    // Convenience skip — walks past matched braces/brackets without
    // materializing anything. Cost = pure scan.
    public void skipValue();

    // Materialize the subtree under the cursor into a JsonValue tree.
    // Returns transferred ownership; the reader advances past it.
    public #JsonValue readValue();

    // Position info for error reporting.
    public int64 position();
    public int32 line();
    public int32 column();
}

public class JsonReaderOptions {
    public boolean relaxed;               // // and /* */ comments, trailing commas
    public boolean validateUtf8;          // default true; off for trusted input
    public int32 maxDepth;                // default 1024; OOM defense
    public int32 maxStringBytes;          // default 16 MiB
}
```

### Reader doctrine

- **No allocation in the steady state.** `next()` advances a cursor
  and updates an internal state machine; it does not allocate.
  `currentString()` only allocates when the underlying span contains
  escape sequences requiring decoding — pure ASCII strings return a
  borrowed slice of the input buffer wrapped as a `String` view (zero
  copy).
- **The cursor model is one-shot.** The reader walks the input once
  forward. There is no back-up, no two-token lookahead exposed. Users
  who need that build it on top with their own buffering layer.
- **Errors are recoverable.** Malformed input throws
  `JsonParseException` (a `RecoverableException` — see ErrorModel.md);
  the reader's state after the throw is undefined and the caller
  should discard it. No partial-result API.

### Sample — count top-level keys without allocating

```cajeta
JsonReader r = heap JsonReader(input);
int32 count = 0;
if (r.next() != JsonToken.START_OBJECT) {
    throw heap JsonParseException("expected object", r.position());
}
while (true) {
    JsonToken t = r.next();
    if (t == JsonToken.END_OBJECT) { break; }
    if (t != JsonToken.KEY) {
        throw heap JsonParseException("expected key", r.position());
    }
    count = count + 1;
    r.next();         // advance to the value
    r.skipValue();    // discard it
}
return count;
```

Zero heap allocations beyond the reader itself. The whole input is
walked once.

---

## Tier 2 — pull writer

```cajeta
public class JsonWriter {
    public JsonWriter();                  // builds into an internal buffer
    public JsonWriter(JsonWriterOptions opts);

    public JsonWriter beginObject();      // {
    public JsonWriter endObject();        // }
    public JsonWriter beginArray();       // [
    public JsonWriter endArray();         // ]
    public JsonWriter key(String k);      // "k":  (writes the colon too)

    public JsonWriter writeNull();
    public JsonWriter writeBoolean(boolean v);
    public JsonWriter writeNumber(int32 v);
    public JsonWriter writeNumber(int64 v);
    public JsonWriter writeNumber(float64 v);
    public JsonWriter writeString(String v);
    public JsonWriter writeRaw(byte[] preformatted);   // user-vouched-for JSON

    // Materialize. Transfers the built buffer to the caller; the
    // writer is reset to empty.
    public #byte[] toBytes();
    public #String toString();
}

public class JsonWriterOptions {
    public boolean pretty;                // default false
    public int32 indentSpaces;            // default 2 when pretty
    public boolean asciiSafe;             // escape U+0080+; default false
}
```

### Writer doctrine

- **Builder-pattern fluent chain.** Every method returns `this` so
  call sites read top-to-bottom. The borrow checker accepts this
  because the receiver pointer is unchanged across the chain.
- **No intermediate strings.** Number formatting writes ASCII digits
  directly to the output buffer (`grisu` for floats, lookup-table-
  driven for ints). The writer never constructs a `String` for a
  primitive's textual form.
- **Single growable buffer.** Backed by an internally-owned `#byte[]`
  with geometric growth (×2). The buffer is transferred out on
  `toBytes()`; the writer keeps no reference. Re-use a writer for
  many documents by calling `toBytes()` (which both transfers and
  resets) at the boundary.
- **Pretty-print is opt-in and slower.** v1 emits compact output by
  default. The `pretty` flag inserts newlines and indentation; v1
  doesn't promise the same throughput as compact mode.

### Sample — write `{"id":42,"tags":["a","b"]}` with no temporaries

```cajeta
JsonWriter w = heap JsonWriter();
w.beginObject()
 .key("id").writeNumber(42)
 .key("tags").beginArray()
   .writeString("a").writeString("b")
 .endArray()
 .endObject();
#byte[] out = w.toBytes();
```

---

## `Json` factory — entry points

```cajeta
package cajeta.codec.json;

public final class Json {
    // Tier 1 — codegen path. Method-level templates; each call site
    // monomorphizes the per-field reader / writer code for T at
    // compile time.
    public static final <T> #T parse(byte[] bytes);
    public static final <T> #T parse(byte[] bytes, JsonReaderOptions opts);
    public static final <T> #byte[] toBytes(T value);
    public static final <T> #byte[] toBytes(T value, JsonWriterOptions opts);

    // Tier 3 sugar — same as `heap JsonReader(bytes).readValue()`.
    public static #JsonValue parse(byte[] bytes);
}
```

For Tier 1 (`<T>` form), `T` must be a class type. Primitives can't
appear at the top level because JSON requires a root value; if a
user wants `Json.parse<int32>("42")`-style top-level primitives,
they go through Tier 2 (`JsonReader.next()` + `r.currentNumber()
.asInt32()`).

For Tier 3 (no template arg), the call returns the generic
`#JsonValue` tree.

---

## Tier 3 — value tree (the `JsonValue` API)

For ad-hoc work where convenience beats throughput. Parse a full
document into a tree, walk it with `.asObject().get("k").asArray()...`
chains, mutate, then serialize:

```cajeta
#JsonValue v = JsonValue.parse(input);
JsonObject root = v.asObject();
int32 id = root.get("id").asNumber().asInt32();
root.put("seen", JsonValue.ofBoolean(true));
#byte[] out = v.toBytes();
```

`JsonValue.parse(byte[])` is sugar for `heap
JsonReader(input).readValue()`. `JsonValue.toBytes()` is sugar for
`heap JsonWriter().writeValue(this).toBytes()`. Both pay the full
allocation cost — every primitive becomes its own `JsonValue` shell.
For documents over a few hundred KB, prefer Tier 2.

---

## Tier 1 — `Json.parse<T>` / `Json.toBytes` + field annotations

The fast path. The user writes a plain class — no class-level
annotation, no mention of an encoder type — and calls into the codec
with `T` as a method-level template arg:

```cajeta
public class UserMessage {
    int32 id;
    String name;
    String email;
}

UserMessage u = Json.parse<UserMessage>(jsonBytes);
#byte[] out  = Json.toBytes(u);
```

`Json.parse<T>` and `Json.toBytes` are method-level templates in the
`cajeta.codec.json.Json` factory class. At each instantiation site,
the compiler walks `T`'s declared fields and emits per-field reader /
writer code that talks directly to `JsonReader` / `JsonWriter`. No
`JsonValue` tree, no `Encoder<T>` indirection, no reflection.

What the compiler synthesizes for the call above:

```cajeta
// Conceptual equivalent of the synthesized Json.parse<UserMessage> body:
public static #UserMessage parse_UserMessage(byte[] bytes) {
    JsonReader r = heap JsonReader(bytes);
    #UserMessage out = heap UserMessage();
    if (r.next() != JsonToken.START_OBJECT) { throw heap JsonParseException(...); }
    while (true) {
        JsonToken t = r.next();
        if (t == JsonToken.END_OBJECT) { break; }
        if (t != JsonToken.KEY) { throw heap JsonParseException(...); }
        String k = r.currentKey();
        if (k == "id")         { r.next(); out.id    = r.currentNumber().asInt32(); }
        else if (k == "name")  { r.next(); out.name  = r.currentString(); }
        else if (k == "email") { r.next(); out.email = r.currentString(); }
        else                   { r.next(); r.skipValue(); }
    }
    return out;
}

// Conceptual equivalent of the synthesized Json.toBytes(UserMessage):
public static #byte[] toBytes_UserMessage(UserMessage value) {
    JsonWriter w = heap JsonWriter();
    w.beginObject()
     .key("id").writeNumber(value.id)
     .key("name").writeString(value.name)
     .key("email").writeString(value.email)
     .endObject();
    return w.toBytes();
}
```

The key dispatch is an `if/else` chain in declaration order (v1; v2
can swap to a perfect-hash table once field count exceeds a
threshold). Each field is read or written in a single call.

### Field-level annotations

The synthesizer is steered by per-field annotations:

```cajeta
public class UserMessage {
    @JsonProperty("user_id")
    int32 id;                            // wire key is "user_id"

    String name;                         // wire key is "name" (default)

    @JsonIgnore
    String passwordHash;                 // skipped on read AND write

    @JsonRequired
    String email;                        // throws if missing during parse

    @JsonInclude(NON_NULL)
    String optionalNote;                 // omitted from output when null
}
```

Annotation surface (all in package `cajeta.codec.json`):

- **`@JsonProperty(String name)`** — overrides the wire-key for this
  field. Without it, the field's declared name is used verbatim.
- **`@JsonIgnore`** — synthesizer skips this field for both read and
  write. The field's slot still exists in memory; the codec just
  pretends it isn't there.
- **`@JsonIgnore(onRead = true, onWrite = false)`** — asymmetric
  variant. `onWrite = false, onRead = true` lets a field be set from
  external input but never echoed back. The flipped pair (read-only-
  output, write-ignored-from-input) is the common audit / computed-
  field pattern.
- **`@JsonRequired`** — synthesizer emits a `if (!sawKey) throw
  heap JsonRequiredFieldException("email")` check at end-of-object.
  Without this, missing keys leave the field at its default.
- **`@JsonInclude(NEVER | ALWAYS | NON_NULL | NON_DEFAULT)`** —
  controls when the writer emits this field. `ALWAYS` (default)
  always writes; `NON_NULL` omits null class values; `NON_DEFAULT`
  omits primitive zeros / boolean false / empty collections;
  `NEVER` mirrors `@JsonIgnore(onWrite=true)`.
- **`@JsonAlias({"alt1", "alt2"})`** — accepts these keys *in
  addition* to the primary name during parse. Writes always emit the
  primary name. Useful for backward compatibility with renamed
  fields.
- **`@JsonRaw`** — the field's wire-value is treated as a pre-
  formatted JSON byte slice on both sides. The synthesizer copies
  bytes verbatim during write and captures the token's raw byte
  slice during read. Used for embedded blobs the application doesn't
  want to parse twice.

### Class-level naming strategy

`@JsonNamingStrategy(SNAKE_CASE | CAMEL_CASE | KEBAB_CASE |
PASCAL_CASE | IDENTITY)` on the class applies a transform to every
field name that doesn't carry an explicit `@JsonProperty`:

```cajeta
@JsonNamingStrategy(SNAKE_CASE)
public class UserMessage {
    int32 userId;        // wire key: "user_id"
    String firstName;    // wire key: "first_name"
    @JsonProperty("email")  // explicit override survives
    String emailAddress; // wire key: "email"
}
```

`IDENTITY` (default) is the no-op transform. Strategies are pure
functions of the field name; the compiler resolves them at
synthesis time, so there's no per-call runtime cost.

### Unknown-key policy

The synthesizer accepts unknown keys silently by default
(`r.skipValue()` branch above) — JSON parsers in the wild encounter
many incoming-field-but-not-modeled cases, and crashing the parse on
each is hostile. The class-level annotation `@JsonStrict` flips the
policy: unknown keys throw `JsonUnknownFieldException` carrying the
offending key. Use this for closed-schema interchange where extra
fields indicate a producer/consumer mismatch.

### Optional / nullable fields

A `String name` field whose corresponding JSON key is missing
receives no assignment — the default-initialized value (`null` for
class types, `0` / `false` for primitives) stands. Explicit `null`
in the JSON input (`"name": null`) hits the same branch as missing
— the field stays at its default. Users who need to distinguish
"absent" from "present-as-null" declare the field as
`Optional<String>` (v2 — the synthesizer needs to know to set
`Optional.empty()` vs `Optional.of(null)`).

### Why not `@Encoding(JsonEncoder<T>)`

The binary-format `@Encoding` design in `cajeta.wire` (see
Annotations.md § @Encoding) is the right shape for formats where the
*encoder is the format* — MessagePack, Protobuf, Avro all need a
class-level binding because the wire bytes are opaque without one.
JSON is different: the format is fixed, and what varies between
classes is the field-name mapping and the include/exclude/required
behavior. Pushing that mapping into a generic `Encoder<T>` would
either force every JSON-using class into a `@Encoding` annotation
(noise — the class name repeats, the encoder type name is
boilerplate), or force the encoder to do reflection at runtime
(slow). The method-template-plus-field-annotation design avoids
both: zero class-level annotation when defaults suffice, per-field
control when not, and the codegen is monomorphic per `T`.

`cajeta.wire.JsonEncoder<T>` is therefore **not specified by this
doc**. Annotations.md § @Encoding's existing mention of it is
superseded — JSON uses the codec-direct path. Binary formats keep
`@Encoding`.

---

## Number representation

JSON numbers are arbitrary-precision in the spec, but real users want
`int32` / `int64` / `float64`. The library splits the difference:

- **Storage.** The raw byte slice of the number's text is kept in
  `JsonNumber`. No parse cost is paid until the caller asks for a
  concrete type.
- **Integer fast path.** If `isInteger()` (no `.`, `e`, or `E` in the
  slice), `asInt64()` does a single-pass digit accumulation with
  overflow check; throws on overflow.
- **Float path.** `asFloat64()` uses the same algorithm a competent
  C standard library does (`strtod`-class). Round-trip for
  doubles is required: parsing then writing the same value yields
  byte-identical output for the canonical short form.
- **No bigints in v1.** A document containing `99999999999999999999`
  parses as a `JsonNumber` whose `asInt64()` throws
  `JsonOverflowException` and whose `asFloat64()` returns the nearest
  double. A `JsonBigInteger` type can land later if a real use case
  emerges.

---

## String handling

- **UTF-8 is the wire encoding.** The reader accepts UTF-8 bytes; the
  writer emits UTF-8 bytes. No UTF-16 or UTF-32 interchange.
- **Escape decoding is lazy.** The reader records the *bounds* of a
  string token without scanning for escapes. `currentString()` scans
  once: if no escapes, returns a borrowed slice of the input (zero
  copy); if escapes, allocates a new `String` and resolves them.
- **Validation.** `validateUtf8=true` (default) checks well-formedness
  during string-token consumption. `validateUtf8=false` skips —
  appropriate when the input comes from a trusted source (an internal
  RPC, a file written by Cajeta) and the cost matters.
- **Surrogate handling.** `\uD800` through `\uDFFF` lone surrogates
  are rejected per RFC 8259 § 8.2 (well-formed). Surrogate pairs
  (`😀`) decode to the corresponding codepoint
  (U+1F600 here).

---

## Error model

All parse / type-coercion failures throw subtypes of
`JsonException extends RecoverableException` (per ErrorModel.md):

- `JsonParseException` — malformed input. Carries byte offset,
  line, column, and a brief message ("unexpected character `}`",
  "unterminated string", "invalid escape `\\q`", etc.).
- `JsonTypeException` — `asNumber()` called on a STRING, etc.
  Carries the offending kind and the requested kind.
- `JsonOverflowException` — `asInt32()` on a number that doesn't
  fit. Carries the offending text and the target type.
- `JsonDepthException` — input nests deeper than `maxDepth`. Carries
  the depth at which the limit was hit.

Errors are thrown, never returned in an out-parameter. The reader's
state after a throw is undefined; discard the reader.

---

## RFC 8259 conformance

- **Strict mode** (default): accepts exactly the grammar in RFC 8259
  § 2. Trailing commas are rejected. Unquoted keys are rejected.
  Comments are rejected. Numbers must match the spec's number
  production.
- **Relaxed mode** (`JsonReaderOptions.relaxed = true`): adds
  trailing-comma tolerance in objects and arrays, and accepts `//
  line` and `/* block */` comments at any whitespace position. Used
  for hand-edited configuration; explicitly NOT a wire-format mode.
- **Duplicate keys.** RFC 8259 says the behavior is implementation-
  defined. Cajeta keeps the last value seen (matches JavaScript and
  most Java parsers). `JsonReaderOptions.rejectDuplicateKeys = true`
  promotes duplicates to `JsonParseException` for callers that
  treat duplicates as a structural error.

---

## Memory model interactions

- `JsonValue.parse(byte[])` returns `#JsonValue` (ownership
  transferred to caller). Drop reclaims the entire tree via the
  standard auto-field-drop chain.
- `JsonReader.currentString()` returns a `String` that may be a
  borrowed view into the input buffer (zero-copy fast path) or an
  owned heap String (escapes present). The user can't distinguish at
  call sites that just need to read; for cross-call-boundary use the
  receiver doctrine of `String` already covers the borrow rules.
- `JsonWriter.toBytes()` returns `#byte[]` — ownership transferred,
  writer's buffer reset. Calling `toBytes()` twice on one writer
  without another `beginObject` between them yields an empty buffer
  the second time (no error — the writer is now in the initial
  empty state).
- `JsonObject` / `JsonArray` are stream-shaped owners; iterating
  via the `Stream<...>` protocol borrows each entry for the
  iteration body (the standard Stream lifetime rule).

---

## Performance notes (v1 target, v2 directions)

### v1 — competitive scalar baseline

- Pull tokenizer: ≥ 500 MB/s on simple shapes (single-level objects of
  ASCII strings + integers) on a modern x86_64 core.
- Direct-codegen path (Tier 1): bounded by `JsonReader` throughput
  plus a constant per-field dispatch; aim within 10% of a hand-written
  parser for the same struct.
- Writer: ≥ 800 MB/s compact output; pretty mode ≥ 200 MB/s.

### v2 — future directions

- **SIMD structural scan.** simdjson-style branchless quote/escape
  detection over 32- or 64-byte chunks, then per-token validation.
  Realistic 2–4× speedup on documents dominated by structural
  characters.
- **Perfect-hash key dispatch.** For Tier 1 structs with many fields,
  swap the if/else chain for a compile-time perfect hash of the
  declared keys.
- **Arena allocator for Tier 3 trees.** A `JsonReader.readValueArena()`
  variant that allocates every node into a single arena, freed in
  O(1). Cuts the per-node header cost and gives the GC nothing to
  walk.
- **Streaming writer to an output stream.** Direct write into a
  `cajeta.io.OutputStream` instead of an internal buffer, for
  serializing larger-than-RAM documents.

None of v2 is on the S-1102 critical path; the impl can ship the
scalar baseline first and layer SIMD on later without API breakage.

---

## Out of scope

- **JSON5, JSONC, HJSON.** Relaxed mode covers the common
  configuration cases; full extended-syntax dialects are not stdlib.
- **JSON Pointer (RFC 6901) / JSON Patch (RFC 6902).** Sibling
  libraries; not part of S-1101.
- **Schema validation.** A `cajeta.codec.jsonschema` package can
  land separately if demand exists.
- **JsonPath / JMESPath query.** Same — separate library.

---

## Cross-references

- `docs/stdlib/Annotations.md` § @ToString — consumes
  `Json.toBytes(this)` when `format=TO_STRING_JSON`.
- `docs/stdlib/Annotations.md` § @Encoding — binary-format
  codegen (MessagePack / Protobuf / Avro). JSON does NOT use
  `@Encoding`; see § "Why not `@Encoding(JsonEncoder<T>)`" above.
- `docs/stdlib/ErrorModel.md` — `JsonException`'s place in
  the Recoverable hierarchy.
- `docs/stdlib/Streams.md` — `JsonArray` / `JsonObject`
  multiple-inherit `Stream<...>`.
- `docs/stdlib/MethodLevelTemplate.md` — `Json.parse<T>` /
  `Json.toBytes` follow the standard final-method-template contract.
- `Features.md` S-1101 (this spec), S-1102 (the implementation).
