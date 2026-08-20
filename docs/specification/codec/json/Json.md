# cajeta.codec.json

JSON value model, parser, and writer for the cajeta stdlib. Single
source of truth for what "JSON support" means in Cajeta — consumed by
`@ToString(format=TO_STRING_JSON)` (Annotations.md § @ToString) and
direct `Json.parse<T>(bytes, len)` / `Json.toBytes<T>(value)` calls in
user code. Mapping between JSON keys and class fields is driven by per-
field annotations (`@JsonProperty`, `@JsonIgnore`, `@JsonRequired`,
`@JsonNamingStrategy` at class level); no class-level annotation is
required to make a type JSON-compatible.

Status: **shipped (v1 scalar baseline)**. The pull tokenizer
(`JsonReader`), pull writer (`JsonWriter`), value tree (`JsonValue` /
`JsonObject` / `JsonArray`), the `Json` entry points, and the Tier-1
compile-time synthesizer all exist under
[`runtime/src/cajeta/codec/json/`](../../../../runtime/src/cajeta/codec/json/)
and are covered by tests (`test/parser/Json*Tests.cpp`). The forward
items called out below (options structs, a `JsonNumber` wrapper, escape
*decoding*, relaxed mode) are **planned**, not built — each is flagged
inline. specs/Features.md S-1101 / S-1102.

## Performance at a glance

Measured on one machine (AMD Ryzen AI Max+ 395), MB/s, against Jackson
(Java's reference JSON library) on the standard simdjson corpus. The
SIMD scanner is pure Cajeta on the built-in `Vector<T,N>` (no `@Native`
C) — see `docs/specification/math/Simd.md` and the harness in `bench/`.

| workload | twitter | citm | canada | vs Jackson |
|---|---|---|---|---|
| **Cajeta — `Json.parse<T>` binding** (SIMD index + inline walk) | **~3690** | **~4010** | **~2850** | **2.4× / 2.2× / 3.5×** |
| **Cajeta — full typed tokenize** (KEY/STRING/NUMBER/…) | **~3100** | **~4250** | **~3100** | **2.0× / 2.3× / 3.8×** |
| Cajeta — token count (stage-1 popcounts) | ~3200 | ~3400 | ~3450 | 2.1× / 1.9× / 4.3× |
| Cajeta — stage-1 structural scan (classify only) | ~8000 | ~10900 | ~8200 | — (scan ceiling) |
| Jackson — full tokenize (reference) | 1551 | 1837 | 811 | 1.0× |
| Cajeta — `Json.parse<T>` over the pull reader (superseded) | ~620 | ~700 | ~450 | 0.4× |
| Cajeta — scalar baseline (v1, pre-SIMD) | ~345 | ~395 | ~230 | 0.22–0.28× |

The **`Json.parse<T>` binding** is the headline path: the Tier-1 synthesizer
codegens the deserializer directly over a SIMD *structural index* (stage 1) in
a tight inline walk (stage 2) — dispatching on one byte per token, decoding only
T's mapped fields and skipping unmapped subtrees with no value decode. Crucially
it has **no per-token `JsonReader.next()` call boundary**: that boundary forces
parser state into a heap object the AOT compiler can't register-promote and caps
the pull path at ~0.4× Jackson (the superseded row). Driving the SIMD engine
from a register-state walk carries the scan throughput all the way to the bound
struct — **2.2–3.5× Jackson** (skip-all; real decode adds bounded per-field work
Jackson also pays). See `plans/Json-fast-path.md`.

**Machine & method.**

- **CPU** — AMD Ryzen AI Max+ 395 ("Strix Halo", Zen 5), 16 cores / 32 threads,
  up to 5.19 GHz, with a Radeon 8060S iGPU. Single-threaded benchmark.
- **RAM** — 64 GiB. **OS** — Ubuntu 26.04 LTS, Linux kernel 7.0.0-22-generic.
- **Toolchain** — `cajeta` 0.7.1, `--emit=exe --release` (LLVM O2), built against
  the `cajeta-llvm` fork (LLVM 23). Native AOT, no JIT warmup.
- **Method** — 200 measured iterations after 20 warmup. `Json.parse<T>` borrows
  its input and frees it on drop (one-shot contract), so each iteration parses a
  **fresh copy** and a copy-only baseline is subtracted. Datasets are the
  standard simdjson corpus (`twitter` 617 KB, `citm_catalog` 1.6 MB, `canada`
  2.1 MB). Jackson 2.18.2 measured on the JVM with generous JIT warmup
  (100 iterations) over `byte[]`.

The typed tokenizer emits the **exact** token stream of the pull
`JsonReader` (validated token-for-token and per-type counts against the
reader on all three datasets). The journey from the scalar baseline to
here was three levers: removing a per-call scope-frame `malloc`
(`--lazy-scope`, ~2×), porting the structural scan to SIMD on
`Vector<T,N>` (the bulk of it), and keeping all hot **scanner state in
registers** rather than a heap object (~2× on the typed walk — output
stays heap, state goes to the stack). Net: **~0.25× → 2.0–3.8× Jackson**,
a 10–15× swing.

### Cross-language: Cajeta vs Rust vs Java

Same machine, same methodology (idle box, peak-of-batches MB/s). The
apples-to-apples comparison for the headline path is **structural skip-all** —
fully scan + validate JSON structure and decode/materialize *nothing*. That is
exactly what `Json.parse<BBEmpty>` does (build the SIMD index + depth-walk,
skipping every value), and its direct analogs are Rust `serde_json`'s
`IgnoredAny` (scalar) and Java Jackson's tokenize.

| structural skip-all (build structure, decode nothing) | twitter | citm | canada |
|---|---|---|---|
| **Cajeta — `Json.parse<BBEmpty>`** (SIMD index + walk) | **3817** | **4102** | **2944** |
| Rust — `serde_json` `IgnoredAny` (scalar) | 3452 | 2888 | 2704 |
| Java — Jackson tokenize | 1551 | 1837 | 811 |
| _Cajeta vs serde_json_ | _1.11×_ | _1.42×_ | _1.09×_ |
| _Cajeta vs Jackson_ | _2.46×_ | _2.23×_ | _3.63×_ |

Cajeta edges Rust's `serde_json` `IgnoredAny` on all three and is 2.2–3.6×
Jackson. Note the comparison *favors* serde_json: `IgnoredAny` borrows its input
(no per-iteration copy), whereas the Cajeta number has a fresh-copy baseline
subtracted because `parse` frees its buffer — and Cajeta still wins.

For context, Rust's **full-materialization** paths (which do strictly more work
than skip-all — they build a navigable value tree, so they are *not* directly
comparable to the skip-all row above):

| full materialization (build a value tree) | twitter | citm | canada |
|---|---|---|---|
| Rust — `simd-json` tape (SIMD) | 1712 | 1889 | 916 |
| Rust — `simd-json` owned DOM | 701 | 891 | 537 |
| Rust — `serde_json` `Value` DOM | 538 | 823 | 598 |

`simd-json`'s SIMD tape materializes every value into a flat tape, so it trails
the skip-all structural number — the cost is the materialization, not the SIMD.
A like-for-like SIMD-vs-SIMD *structural* comparison would need simd-json's
stage-1 index alone, which the library doesn't expose as a standalone path.

> Harness: `serde_json` 1.x + `simd-json` 0.13, `-C target-cpu=native`, 200
> iterations peak-of-batches, copy baseline subtracted for `simd-json` (it mutates
> its input in place). Same datasets as above.

## Goals, in priority order

1. **Fast.** Throughput is the design's first constraint, not an
   afterthought. The reader streams tokens without materializing a
   value tree; the writer formats primitives directly into a growable
   byte buffer; the `@Encoding` codegen path bypasses both layers and
   talks field-by-field to the user's struct. The library must be
   competitive with hand-tuned scalar JSON parsers in C and Java (rough
   target: ≥ 500 MB/s for the pull tokenizer on simple shapes on a
   modern x86_64 core — long since cleared; see *Performance at a
   glance* above). SIMD-accelerated structural scanning, once a future
   direction, is now built (pure Cajeta on `Vector<T,N>`) and beats
   Jackson 2–3.8×.
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
│  Tier 1 — Json.parse<T>(bytes, len) / Json.toBytes<T>(value)│  fastest, zero-config
│  Method-level templates; the compiler walks T's declared    │
│  fields and emits per-field reader / writer code that talks │
│  directly to JsonReader / JsonWriter. No JsonValue tree.    │
│  Per-field annotations (@JsonProperty, @JsonIgnore, etc.)   │
│  drive the mapping; no class-level annotation required.     │
└─────────────────────────────────────────────────────────────┘
```

A user writing a server hot-path calls `Json.parse<T>(bytes, len)` —
nothing else required; the compiler does the work. A user writing a
SAX-style streaming consumer drops to Tier 2. A user writing ad-hoc
config inspection picks Tier 3 and pays for the convenience.

---

## Value model — `JsonValue`

`JsonValue` is a flat tagged union: an `int32 kind` discriminator plus
one payload field per kind. The kind tags are `static final int32`
constants on `JsonValue` (there is no separate `JsonKind` enum — keeping
the discriminator a plain `int32` avoids a per-check enum allocation).

```cajeta
package cajeta.codec.json;

public class JsonValue {
    // Kind discriminator constants (compare against `v.kind()`).
    public static final int32 NULL    = 0;
    public static final int32 BOOLEAN = 1;
    public static final int32 NUMBER  = 2;   // int64 payload (v1 — no float in the tree)
    public static final int32 STRING  = 3;   // borrowed int8[] slice + length
    public static final int32 ARRAY   = 4;
    public static final int32 OBJECT  = 5;

    public JsonValue();                  // empty, kind == NULL
    public int32 kind();
    public boolean isNull();             // kind == NULL (no exception)

    // Payload accessors. These do NOT validate the kind — gate on
    // kind() first (or use JsonObject's typed Optional getters). A
    // mismatched read returns the zero/null of that payload field.
    public boolean   asBoolean();        // BOOLEAN
    public int64     asInt64();          // NUMBER
    public int32     asInt32();          // NUMBER, narrowed
    public int8[]    asBytes();          // STRING — borrowed slice (escapes verbatim)
    public int32     asBytesLength();    // STRING — slice length
    public #String   asString();         // STRING → materialized String, or null
    public JsonArray  array();         // ARRAY
    public JsonObject object();         // OBJECT

    // Builders mutate `this` and return it for chaining. The #-typed
    // overloads (setArray/setObject/setStringOwned) take ownership.
    public JsonValue setNull();
    public JsonValue setBoolean(boolean v);
    public JsonValue setNumber(int64 v);
    public JsonValue setString(int8[] bytes, int32 len);   // borrow
    public JsonValue setString(String s);                  // borrow
    public JsonValue setStringOwned(#int8[] bytes, int32 len);
    public JsonValue setArray(#JsonArray a);
    public JsonValue setObject(#JsonObject o);
}
```

### Why a flat int32-tagged union and not class inheritance

Class inheritance (`JsonNull extends JsonValue`, `JsonString extends
JsonValue`, etc.) would require a vtable dispatch on every kind-check
and a per-value heap allocation. The flat-tag form lets the compiler
emit an `if (v.kind() == ...)` ladder for the hot paths and keeps the
layout fixed-size (~48 bytes) regardless of contents. That waste is the
price of the convenience tier; Tiers 1 and 2 bypass `JsonValue`
entirely.

### Numbers in the tree

There is **no `JsonNumber` wrapper class** in v1. A `NUMBER` `JsonValue`
stores an `int64` (`asInt64()` / `asInt32()`); the tree does not yet
carry floats. When you need lazy / float / overflow-checked number
handling, drop to the Tier-2 reader, whose `currentNumberAsInt64()`,
`currentNumberAsInt32()`, and `currentNumberAsFloat64()` parse the token
span on demand (a span the reader records without parsing until asked).
A lazy `JsonNumber` value-tree wrapper is a planned addition.

### `JsonObject` and `JsonArray`

```cajeta
public class JsonArray {
    public JsonArray();
    public int32 count();                  // element count
    public JsonValue get(int32 i);         // no bounds-Optional yet
    public void add(#JsonValue v);         // takes ownership
}

public class JsonObject {
    public JsonObject();
    public int32 count();
    public boolean containsKey(String key);
    public JsonValue get(String key);                // null if absent
    public JsonValue get(int8[] key, int32 keyLen);  // hot-path byte form
    public void put(#int8[] key, int32 keyLen, #JsonValue v);  // takes ownership; no dedup
    public #ArrayList<String> keys();                // insertion order

    // Positional accessors (insertion order) — used by the writer.
    public int8[]    keyAt(int32 i);
    public int32     keyLenAt(int32 i);
    public JsonValue valueAt(int32 i);

    // Typed convenience getters: Optional.empty() when the key is
    // absent OR the value's kind doesn't match (no silent coercion).
    public Optional<String>     getString(String key);
    public Optional<int32>      getInt(String key);
    public Optional<int64>      getLong(String key);
    public Optional<boolean>    getBoolean(String key);
    public Optional<JsonArray>  getArray(String key);
    public Optional<JsonObject> getObject(String key);
    public #ArrayList<String>   getStringArray(String key);   // string elems only
}
```

`JsonArray` is a geometric-growth `JsonValue[]`. `JsonObject` is
insertion-order-preserving and looks keys up by **linear scan** (v1 does
no hashing — the convenience tier isn't the hot path; Tier 1 handles
perf-sensitive shapes field-by-field). `put` does not deduplicate:
re-putting a key appends a second entry `get` will never reach.
`Stream<...>` inheritance (so `filter` / `map` / `forEach` apply) is a
planned addition, not built — there is no `JsonEntry` type and no
`getOpt` yet.

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
    // byteCount is the valid length of `input` (passed explicitly until
    // primitive arrays expose .count() as a property accessor).
    public JsonReader(int8[] input, int64 byteCount);

    public JsonToken next();              // advance to and return next token
    public JsonToken peek();              // one-token lookahead (cached)
    public JsonToken current();           // last token returned

    // Current-token materializers. STRING/KEY both surface through the
    // same byte/string accessors after their token; a KEY is just a
    // STRING in key position, so read it with currentBytes/currentString.
    public boolean   currentBoolean();    // BOOLEAN: true / false
    public #int8[]   currentBytes();      // fresh copy of the token span
    public #int8[]   currentRawBytes();   // span incl. surrounding quotes (for @JsonRaw)
    public #String   currentString();     // token span as a String
    public int64     currentNumberAsInt64();    // NUMBER → int64 (throws on overflow / non-integer)
    public int32     currentNumberAsInt32();     // NUMBER → int64 then narrow
    public float64   currentNumberAsFloat64();   // NUMBER → float64 (naive accumulation)

    // Materialize the value at the cursor into a JsonValue tree.
    // Ownership transfers to the caller; the reader advances past it.
    public #JsonValue readValue();
    public #JsonValue readValueAfter(JsonToken t);   // build from an already-pulled token

    // Position info for error reporting (byte offset; line/column not yet derived).
    public int64 position();
    public int64 tokenStart();
    public int64 tokenEnd();
}
```

There is **no `JsonReaderOptions` struct** in v1: parsing is strict
RFC 8259 only (relaxed mode, UTF-8 validation toggles, and a tunable
`maxStringBytes` are planned). `maxDepth` is fixed at 1024 internally.

### Reader doctrine

- **No allocation in the steady state.** `next()` advances a cursor
  and updates an internal state machine; it does not allocate. String
  and number tokens are recorded as `(tokenStart, tokenEnd)` byte spans
  — the caller pays the copy/parse cost only on `currentBytes()` /
  `currentString()` / `currentNumberAs*()`. Note v1 `currentBytes()` /
  `currentString()` **copy** the span and keep escape sequences verbatim
  (`\n` stays the two bytes `\` `n`); zero-copy String views and escape
  *decoding* are planned.
- **The cursor model is forward-only, with one-token lookahead.**
  `peek()` caches the next token so the Tier-1 synthesizer can dispatch
  array elements; otherwise the reader walks the input once forward with
  no back-up.
- **Errors are recoverable.** Malformed input — and integer overflow or
  exceeding the depth limit — throws `JsonParseException` (a
  `RecoverableException`, see ErrorModel.md); the reader's state after
  the throw is undefined and the caller should discard it. No
  partial-result API.

### Sample — count top-level keys without allocating

```cajeta
JsonReader r = heap JsonReader(input, (int64) input.count());
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
    r.next();           // advance to the value
    // v1 has no recursive skipValue(); for a scalar value the next()
    // above already consumed it. (Recursive skip lands with peek-based
    // subtree skipping.)
}
return count;
```

The reader itself is the only allocation; the whole input is walked
once.

---

## Tier 2 — SAX push parser (`JsonSax` / `JsonHandler`)

The push counterpart to the pull reader: subclass `JsonHandler`,
override only the events you care about (every method is a no-op by
default), and hand it to `JsonSax.parse`. The parser drives the document
once and calls your events in document order — memory is O(nesting
depth), independent of document size.

```cajeta
public class JsonHandler {
    public void onStartObject();           public void onEndObject();
    public void onStartArray();            public void onEndArray();
    public void onKey(JsonReader r);       // pull: r.currentString()
    public void onString(JsonReader r);    // pull: r.currentString()
    public void onNumber(JsonReader r);    // pull: r.currentNumberAsInt64() / ...AsFloat64()
    public void onBoolean(boolean v);      public void onNull();
}
public final class JsonSax {
    public static void parse(int8[] bytes, int64 byteCount, JsonHandler h);
}
```

Values are delivered **lazily** — the value events receive the
`JsonReader` positioned at the token, so a handler pays to decode only
what it pulls (skipping a value costs nothing). `BOOLEAN`/`NULL` carry no
span, so the boolean is passed directly and `onNull` takes no argument.

```cajeta
public class KeyCounter extends JsonHandler {
    public int64 keys;
    public KeyCounter() { this.keys = 0; }
    public void onKey(JsonReader r) { this.keys = this.keys + 1; }
}
KeyCounter c = heap KeyCounter();
JsonSax.parse(bytes, length, c);     // c.keys == number of object keys
```

`JsonSax` is a thin driver over `JsonReader`, so push and pull emit the
identical token stream; the SIMD scanner (*Performance at a glance*) is a
drop-in speed upgrade for the driver loop. Use SAX when a callback shape
fits the consumer; use the pull reader when you want to navigate (skip
subtrees, peek) yourself.

---

## Tier 2 — pull writer

```cajeta
public class JsonWriter {
    public JsonWriter();                  // builds into a growable internal buffer

    public JsonWriter beginObject();      // {
    public JsonWriter endObject();        // }
    public JsonWriter beginArray();       // [
    public JsonWriter endArray();         // ]
    public JsonWriter key(String name);   // "name":  (writes the colon too)
    public JsonWriter key(int8[] name, int32 n);   // byte-buffer form (synthesizer hot path)

    public JsonWriter writeNull();
    public JsonWriter writeBoolean(boolean v);
    public JsonWriter writeNumber(int64 v);
    public JsonWriter writeNumber(float64 v);       // always with a '.' so it round-trips as float
    public JsonWriter writeString(String v);
    public JsonWriter writeString(int8[] s, int32 n);
    public JsonWriter writeRaw(int8[] bytes, int32 n);   // user-vouched-for JSON, copied verbatim
    public JsonWriter writeValue(JsonValue v);           // re-emit a parsed tree

    public int32 size();                  // bytes written so far

    // Materialize: copies the built document out as a fresh #int8[] and
    // resets the writer (size/depth/key state) for reuse.
    public #int8[] toBytes();
}
```

There is **no `JsonWriterOptions` struct** and **no `toString()`** in
v1: output is always compact. Pretty-printing (`pretty` / `indentSpaces`)
and ASCII-safe escaping (`asciiSafe`) are planned. `writeNumber(int32)`
is not a separate overload — widen to `int64`.

### Writer doctrine

- **Builder-pattern fluent chain.** Every `begin*`/`end*`/`write*`/`key`
  method returns `this`, so call sites read top-to-bottom and the writer
  threads commas/colons itself.
- **No intermediate strings.** Number formatting writes ASCII digits
  directly to the output buffer. v1's `writeNumber(float64)` is a naive
  fixed-precision serializer (integer part + up to 6 fractional digits,
  no scientific notation yet); a strtod-class formatter is planned.
- **Single growable buffer.** Backed by an internally-owned `#int8[]`
  with geometric growth (×2). The buffer is transferred out on
  `toBytes()`, which also resets the writer — reuse one writer across
  many documents by calling `toBytes()` at each boundary. (Calling it
  twice with no intervening writes yields an empty buffer the second
  time.)
- **String escaping is minimal in v1.** `writeString` escapes `"` and
  `\`; it does not yet escape control characters or emit `\u` sequences.

### Sample — write `{"id":42,"tags":["a","b"]}` with no temporaries

```cajeta
JsonWriter w = heap JsonWriter();
w.beginObject()
 .key("id").writeNumber((int64) 42)
 .key("tags").beginArray()
   .writeString("a").writeString("b")
 .endArray()
 .endObject();
#int8[] out = w.toBytes();
```

### JSON Lines (`JsonLinesWriter`) — newline-delimited records

For `.jsonl` output (one JSON value per line — the structured-logging
wire format), `JsonLinesWriter` wraps a `FileWriter` sink and a reused
`JsonWriter`. It exposes the same fluent build API; `endLine` commits the
current record (appends `\n`, streams the line to the sink) and resets
for the next. The internal buffer is reused across every record — **no
per-line allocation**, and each record is a single `write`.

```cajeta
JsonLinesWriter jl = heap JsonLinesWriter(fileWriter);
jl.beginObject()
    .key("level").writeString("INFO")
    .key("msg").writeString("started")
    .key("ts").writeNumber(epochMillis)
  .endObject()
  .endLine();          // emits {"level":"INFO","msg":"started","ts":...}\n
jl.flush();
```

The same zero-copy path is available directly on `JsonWriter`:
`writeTo(FileWriter)` streams the encoded document to a sink and resets
(no `toBytes` copy); `writeLineTo(FileWriter)` does the same with a
trailing newline. Reach for these on hot paths where `toBytes`' per-call
`int8[]` copy would dominate.

---

## `Json` factory — entry points

```cajeta
package cajeta.codec.json;

public final class Json {
    // Tier 3 — value-tree. Parse a byte buffer (with explicit length)
    // or a String; emit a tree back to bytes.
    public static #JsonValue parse(int8[] bytes, int64 length);
    public static #JsonValue parse(String s);
    public static #int8[]    toBytes(JsonValue value);

    // Tier 1 — codegen path. Method-level templates; each call site
    // monomorphizes the per-field reader / writer code for T at
    // compile time. The captured bodies throw JsonParseException as a
    // failsafe if the synthesizer fails to engage.
    public static T        parse<T>(int8[] bytes, int64 length);
    public static T        parse<T>(String s);
    public static int8[]   toBytes<T>(T value);

    // Synthesizer recursion helpers (public so emitted cross-class
    // code can reach them; not called directly by users).
    public static T    parseObjectFromReader<T>(JsonReader r);
    public static void toBytesObjectInto<T>(JsonWriter w, T value);
}
```

The resolver distinguishes Tier 1 from Tier 3 by the call site's
explicit type argument: `Json.parse(buf, n)` routes to Tier 3 (returns
`#JsonValue`); `Json.parse<T>(buf, n)` routes to the synthesizer
(returns `T`). For Tier 1, `T` must be a class type — JSON requires a
root value, so top-level primitives go through Tier 2
(`JsonReader.next()` + `r.currentNumberAsInt32()`).

---

## Tier 3 — value tree (the `JsonValue` API)

For ad-hoc work where convenience beats throughput. Parse a full
document into a tree, walk it with `.object().get("k").array()...`
chains, mutate, then serialize:

```cajeta
JsonValue v #= Json.parse(input, (int64) input.count());
JsonObject root = v.object();
int32 id = root.get("id").asInt32();
root.put(#keyBytes, keyLen, heap JsonValue().setBoolean(true));
#int8[] out = Json.toBytes(v);
```

`Json.parse(bytes, len)` is sugar for `heap JsonReader(bytes,
len).readValue()`; `Json.parse(String)` forwards to it against the
String's UTF-8 payload. `Json.toBytes(v)` is sugar for `heap
JsonWriter().writeValue(v).toBytes()`. Both pay the full allocation cost
— every primitive becomes its own `JsonValue` shell. For documents over
a few hundred KB, prefer Tier 2. (`JsonObject.put` takes raw key bytes +
length; the typed `getString`/`getInt`/… getters above are the ergonomic
read side.)

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

UserMessage u #= Json.parse<UserMessage>(jsonBytes, (int64) jsonBytes.count());
#int8[] out  = Json.toBytes<UserMessage>(u);
```

`Json.parse<T>` and `Json.toBytes` are method-level templates in the
`cajeta.codec.json.Json` factory class. At each instantiation site,
the compiler walks `T`'s declared fields and emits per-field reader /
writer code that talks directly to `JsonReader` / `JsonWriter`. No
`JsonValue` tree, no `Encoder<T>` indirection, no reflection.

What the compiler synthesizes for the call above:

```cajeta
// Conceptual equivalent of the synthesized Json.parse<UserMessage> body:
public static #UserMessage parse_UserMessage(int8[] bytes, int64 length) {
    JsonReader r = heap JsonReader(bytes, length);
    UserMessage out = heap UserMessage();
    if (r.next() != JsonToken.START_OBJECT) { throw heap JsonParseException(...); }
    while (true) {
        JsonToken t = r.next();
        if (t == JsonToken.END_OBJECT) { break; }
        if (t != JsonToken.KEY) { throw heap JsonParseException(...); }
        String k #= r.currentString();              // KEY token bytes
        if (k == "id")         { r.next(); out.id    = r.currentNumberAsInt32(); }
        else if (k == "name")  { r.next(); out.name  = r.currentString(); }
        else if (k == "email") { r.next(); out.email = r.currentString(); }
        else                   { r.next(); }       // unknown key — value consumed, discarded
    }
    return out;
}

// Conceptual equivalent of the synthesized Json.toBytes(UserMessage):
public static #int8[] toBytes_UserMessage(UserMessage value) {
    JsonWriter w = heap JsonWriter();
    w.beginObject()
     .key("id").writeNumber((int64) value.id)
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

    @JsonInclude("NON_NULL")
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
- **`@JsonRequired`** — the synthesizer tracks a per-field `sawKey`
  flag and throws `JsonParseException` at end-of-object if a required
  key never appeared. Without it, missing keys leave the field at its
  default. (`@JsonIgnore` overrides `@JsonRequired` — an un-read field
  can't be required.)
- **`@JsonInclude("NEVER" | "ALWAYS" | "NON_NULL" | "NON_DEFAULT")`** —
  controls when the writer emits this field (string-valued argument).
  `ALWAYS` (default) always writes; `NON_NULL` omits null class values;
  `NON_DEFAULT` omits a field equal to its type default; `NEVER` makes
  the field read-only (parsed but never written).
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

`@JsonNamingStrategy("SNAKE_CASE" | "CAMEL_CASE" | "KEBAB_CASE" |
"PASCAL_CASE" | "IDENTITY")` on the class applies a transform to every
field name that doesn't carry an explicit `@JsonProperty` (string-valued
argument):

```cajeta
@JsonNamingStrategy("SNAKE_CASE")
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

The synthesizer accepts unknown keys silently by default (the `else {
r.next(); }` arm above) — JSON parsers in the wild encounter many
incoming-field-but-not-modeled cases, and crashing the parse on each is
hostile. The class-level annotation `@JsonStrict` flips the policy:
unknown keys throw `JsonParseException` (message names the unknown key).
Use this for closed-schema interchange where extra fields indicate a
producer/consumer mismatch.

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
`int32` / `int64` / `float64`. The library splits the difference at the
reader:

- **Span, not value.** The tokenizer records a number's `(tokenStart,
  tokenEnd)` byte span without parsing it. The parse cost is paid only
  when the caller calls a `currentNumberAs*()` accessor.
- **Integer path.** `currentNumberAsInt64()` does a single-pass digit
  accumulation (in `uint64`, to dodge the signed-overflow trap) with a
  range check against `INT64_MAX` / `abs(INT64_MIN)`; it throws
  `JsonParseException` on overflow or on a fractional/exponent token.
  `currentNumberAsInt32()` delegates and narrows.
- **Float path.** `currentNumberAsFloat64()` is a naive accumulation
  (integer part, optional fraction, optional exponent) — **not**
  strtod-class accuracy. It round-trips cleanly for typical small-
  magnitude JSON numbers against the writer's fixed-precision output;
  exact round-tripping awaits a strtod-equivalent runtime helper.
- **The value tree stores `int64` only.** `readValueAfter` materializes
  every `NUMBER` node via `currentNumberAsInt64()`, so a fractional
  literal in a Tier-3 parse throws. Float support in the tree, a lazy
  `JsonNumber` wrapper, a dedicated `JsonOverflowException`, and bigints
  are all planned, not built — v1 surfaces overflow as
  `JsonParseException`.

---

## String handling

- **UTF-8 is the wire encoding.** The reader accepts UTF-8 bytes; the
  writer emits UTF-8 bytes. No UTF-16 or UTF-32 interchange.
- **Escapes are NOT decoded in v1.** The reader records the *bounds* of
  a string token, skipping over `\`-escapes without resolving them.
  `currentBytes()` / `currentString()` copy the span **verbatim** — a
  `\n` in the source stays the two bytes `\` `n`. Lazy escape *decoding*
  (and a zero-copy borrowed-slice fast path for escape-free strings) is
  planned.
- **No UTF-8 validation toggle.** There is no `validateUtf8` option yet;
  the reader does not validate string well-formedness.
- **Surrogate handling is planned.** Lone-surrogate rejection and
  surrogate-pair decoding land with the escape-decoder above.

---

## Error model

v1 funnels **all** failures through a single
`JsonParseException extends RecoverableException` (per ErrorModel.md):

```cajeta
public class JsonParseException extends RecoverableException {
    public int64 position;          // 0-based byte offset of the fault
    public JsonParseException(String message, int64 position);
}
```

It carries a brief message ("unexpected character", "unterminated
string", "number out of int64 range", "nesting depth limit exceeded",
"unknown key (class is @JsonStrict)", a missing-`@JsonRequired`-field
message, …) plus the byte offset (`position`). Line/column derivation is
deferred — `position` is the offset only.

The finer-grained subtypes a Jackson-style API would expose —
`JsonTypeException` (wrong-kind access), `JsonOverflowException`
(`asInt32` on an out-of-range number), `JsonDepthException`,
`JsonRequiredFieldException`, `JsonUnknownFieldException` — are
**planned**, not built; today every one of those conditions raises
`JsonParseException`.

Errors are thrown, never returned in an out-parameter. The reader's
state after a throw is undefined; discard the reader.

---

## RFC 8259 conformance

- **Strict mode** (the only v1 mode): accepts exactly the grammar in
  RFC 8259 § 2. Trailing commas are rejected. Unquoted keys are
  rejected. Comments are rejected. Numbers must match the spec's number
  production. (String *content* validation — escape and UTF-8
  well-formedness — is not yet enforced; see § String handling.)
- **Relaxed mode** — trailing-comma tolerance and `//` / `/* */`
  comment support — is **planned**, gated on the `JsonReaderOptions`
  struct that doesn't exist yet. Intended for hand-edited
  configuration; explicitly not a wire-format mode.
- **Duplicate keys.** RFC 8259 leaves this implementation-defined. The
  Tier-3 `JsonObject` does not deduplicate — it appends every entry in
  insertion order, and `get` returns the **first** match. A
  reject-duplicates option is planned.

---

## Memory model interactions

- `Json.parse(bytes, len)` returns `#JsonValue` (ownership transferred
  to the caller). Drop reclaims the entire tree via the standard
  auto-field-drop chain. `readValueAfter` builds string nodes with
  `setStringOwned`, so the tree owns its string bytes.
- `JsonReader.currentString()` / `currentBytes()` return owned `#`
  copies of the token span in v1 (no borrowed-view fast path yet — see
  § String handling). `JsonValue.asString()` materializes a fresh
  `#String` over the value's stored byte slice.
- `JsonWriter.toBytes()` returns `#int8[]` — ownership transferred,
  writer's buffer reset. Calling `toBytes()` twice without intervening
  writes yields an empty buffer the second time (no error — the writer
  is back in its initial empty state).
- `JsonObject` / `JsonArray` own their entries (`put` / `add` take
  `#`-transferred values) and reclaim them on drop. Iterate `JsonArray`
  by index (`get(i)` over `count()`) and `JsonObject` positionally
  (`keyAt` / `valueAt` over `count()`) or by key (`get` / the typed
  getters); `Stream<...>`-protocol iteration is planned.

---

## Performance notes (v1 target, v2 directions)

### v1 — competitive scalar baseline

- Pull tokenizer: ≥ 500 MB/s on simple shapes (single-level objects of
  ASCII strings + integers) on a modern x86_64 core.
- Direct-codegen path (Tier 1): bounded by `JsonReader` throughput
  plus a constant per-field dispatch; aim within 10% of a hand-written
  parser for the same struct.
- Writer: ≥ 800 MB/s compact output; pretty mode ≥ 200 MB/s.

### v1 — measured (streaming tokenize, 2026-06)

First same-machine baseline against the canonical
[nativejson-benchmark](https://github.com/miloyip/nativejson-benchmark) corpus
(`twitter` / `citm_catalog` / `canada`). Workload is **streaming tokenization**:
pull `JsonReader.next()` to `END`, counting tokens, numbers read lazily (no DOM).
Compared against Jackson and Gson running the equivalent streaming loop
(`JsonParser.nextToken()` / Gson `JsonReader` walk) on the **same files**. All
three implementations produced **identical token counts** (twitter 29 573, citm
85 035, canada 223 236) — a correctness cross-check.

- **Machine:** AMD Ryzen AI Max+ 395 (Zen 5), Linux. Single thread.
- **Cajeta:** `--release` native AOT (no warmup needed); per-iteration fresh
  read, read-only baseline subtracted to isolate tokenization.
- **Java:** OpenJDK 25 (Corretto), Jackson 2.18.2 / Gson 2.11.0, JIT warmed
  (100 iters) then 200 measured; bytes read once, parsed in-memory.

| file | size | **Cajeta** | Jackson | Gson | Cajeta vs Jackson | Cajeta vs Gson |
|---|---|---|---|---|---|---|
| twitter | 0.63 MB | **~377 MB/s** | ~1560 | ~485 | 0.24× | 0.78× |
| citm_catalog | 1.73 MB | **~441 MB/s** | ~1810 | ~915 | 0.24× | 0.48× |
| canada | 2.25 MB | **~257 MB/s** | ~810 | ~480 | 0.32× | 0.54× |

**Reading:** tokenization is **Gson-class** (within ~2× of a mature, widely-used
library) and **~3–4× behind Jackson** (best-in-class, byte-level symbol tables +
structural tricks → the v2 SIMD direction below). For a young hand-written scalar
tokenizer this is a credible starting point; the `≥ 500 MB/s` target above holds
only for the simplest single-level integer/ASCII shapes, not the structure- and
number-dense real corpus.

**Gaps that outrank speed (found while benchmarking — fix before treating the
codec as a first-class built-in):**

1. **No float parsing in the value tree.** `Json.parse` → `JsonValue` throws on
   any fractional/exponent literal, so `canada.json` (100 % floats) and
   `twitter.json` (44 floats) cannot be DOM-parsed at all (only the lazy
   streaming reader runs, by not converting). See § "Numbers in the tree" — a
   float-carrying tree is the prerequisite, not an optimization.
2. **Tier-3 DOM does not scale.** A full `JsonValue` tree of `citm_catalog`
   (1.73 MB) exhausts the runtime live-allocation set (65 536) and faults — far
   too many simultaneously-live nodes. The v2 arena allocator below is a
   correctness fix here, not just perf.
3. **`JsonReader` aliases its input.** The ctor stores the borrowed `int8[]`
   into an owned field, so a buffer cannot be shared across readers without a
   double-free. Take `#int8[]` (transfer) or hold a non-owning view.

Reproduce: `bench/src/bench/JsonBench.cajeta` (Cajeta) and the Jackson/Gson
harness used to produce the table.

### SIMD scanner — beats Jackson (2026-06)

A simdjson-style scanner in **pure Cajeta** on the built-in `Vector<T,N>`
(`docs/specification/math/Simd.md`): 16-byte block load (`Cajeta.vload16`), compare→movemask
(`Vector.eqMask`), an integer prefix-XOR string mask (escaped/in-string `,{}[]`
correctly excluded), and `popcount64`. The reader's token count is exactly
`#brackets + #strings + #scalars`, each a popcount of a stage-1 mask.

Same machine, full tokenize, MB/s — **token counts identical to the scalar
reader** (twitter 29 573 / citm 85 035 / canada 223 236):

| file | **Cajeta SIMD** | Jackson | **vs Jackson** |
|---|---|---|---|
| twitter | **~3265** | ~1551 | **2.1×** |
| citm_catalog | **~3181** | ~1837 | **1.7×** |
| canada | **~3463** | ~811 | **4.3×** |

From ~0.45–0.64× (scalar/SWAR) to **1.7–4.3× Jackson** — native SIMD is the lever
the JVM can't pull. Stage-1 classification alone runs 8–11 GB/s. Harness:
`bench/src/bench/TokCount.cajeta` (token count + speed), `Stage1.cajeta` (engine).
The typed-token emitter (KEY/STRING/NUMBER/…) over the same masks is the API
follow-on. `tableLookup`(pshufb)/`clmul`/256-bit widen the margin further.

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

- `docs/specification/reflect/Annotations.md` § @ToString — consumes
  `Json.toBytes(this)` when `format=TO_STRING_JSON`.
- `docs/specification/reflect/Annotations.md` § @Encoding — binary-format
  codegen (MessagePack / Protobuf / Avro). JSON does NOT use
  `@Encoding`; see § "Why not `@Encoding(JsonEncoder<T>)`" above.
- `docs/specification/error/ErrorModel.md` — `JsonException`'s place in
  the Recoverable hierarchy.
- `docs/specification/lang/stream/Streams.md` — `JsonArray` / `JsonObject` are planned to
  multiple-inherit `Stream<...>` (not built; iterate by index/position
  today).
- `docs/specification/lang/templates/MethodLevelTemplate.md` — `Json.parse<T>` /
  `Json.toBytes` follow the standard final-method-template contract.
- `specs/Features.md` S-1101 (this spec), S-1102 (the implementation).
