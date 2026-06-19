# cajeta.codec — Codec & Encoding Framework

The umbrella spec for how Cajeta turns a buffer of bytes into typed data and
back, across wire and storage formats. It defines the **convention** that makes
every codec legible — *the type in your hand names the pipeline stage you are
at, and its methods are the only legal next steps* — and the **abstraction
tiers** (`view`, the streaming readers, `Encoder<T>`, the columnar file/vector
types, the block `Decompressor`) that the per-format libraries implement.

Single source of truth for the codec surface. Consumed by the per-format specs
under `docs/specification/codec/<format>/` (`json/Json.md` is the first and the
proof-of-architecture) and by the `@Encoding` annotation
(`docs/specification/reflect/Annotations.md` § Section 3). Sits directly above
`docs/specification/lang/Views.md` (the terminal zero-copy access tier) and
draws its SIMD/parallelism policy from the compression and SIMD research.

Status: **framework spec (design)**. JSON (`cajeta.codec.json`) ships and proves
the tagged → SIMD-index → typed-walk path (2.0–3.8× Jackson). `Encoder<T>`
ships as an interface (`runtime/src/cajeta/wire/Encoder.cajeta`). Everything else
here — Protobuf, Ion, Avro, CSV, Parquet, ORC, the columnar tier, the
`Decompressor` tier — is **specified, not built**. Each target's status is
flagged inline.

---

## 1. The problem

A "codec" in Cajeta spans a deliberately wide range — from a fixed C-struct
header you overlay in zero copies, to a Parquet file you must locate-decompress-
decode column by column. A single universal mechanism for all of them is **not
viable**: a tightly-packed format like Protobuf or a compressed Parquet page has
no fixed relationship between a field and a byte offset, so you cannot point at
the wire bytes and read a field — the bytes must first be *expanded* into a
representation that does have addressable structure. Conversely, forcing a
fixed-layout network header through a general parser throws away the entire
zero-copy advantage that made it worth a typed overlay.

So the framework's job is **not** to unify the formats under one mechanism. It is
to make it **unambiguous, from the types alone, which mechanism a given format
demands and what steps stand between raw bytes and typed access.** That is the
"well-formed class structure" this spec defines.

### 1.1 The deciding axes

Two orthogonal properties classify every format and select its mechanism:

1. **Offset = f(type)** vs **offset = f(data).** If a field's byte offset is
   fixed by the type declaration alone, the bytes are directly addressable —
   `view` (zero-copy overlay) applies. If the offset depends on the data
   (variable-length, tag-encoded, compressed), a **parse step** is mandatory
   before any typed access.
2. **Row-oriented** vs **columnar**, crossed with **tagged/random-access** vs
   **untagged/linear.** This decides *which* parse mechanism: a lazy index +
   cursor, a linear schema-driven reader, or a multi-stage columnar file reader.

### 1.2 The structural taxonomy

Every format we target lands in one of three classes:

| Class | Property | Formats | Mechanism |
|---|---|---|---|
| **A — Fixed-layout binary** | offset = f(type) | Cap'n Proto, FlatBuffers, custom framed protocols, **decoded leaves/columns** | `view` overlay — zero copy, terminal |
| **B — Self-describing / schema-driven, row** | offset = f(data) | Protobuf, Ion, Avro, JSON, CSV, MessagePack, CBOR | streaming parser: index + cursor (tagged) or linear reader (untagged) |
| **C — Columnar storage container** | offset = f(data), column unit | Parquet, ORC | multi-stage file reader: footer → page/stripe → decompress → decode → column vector |

Class B splits again by random-access capability:

- **Tagged (random-access-capable):** Protobuf, Ion, JSON, MessagePack, CBOR.
  Field tags let us build a structural **index** and lazily decode or *skip*
  individual fields. Supports both an eager typed bind and a lazy cursor.
- **Untagged (linear-only):** Avro, positional CSV. No tags → no random access;
  records must be decoded in declaration/schema order. A linear reader only.

The classification is the whole point: **a format's facade advertises its class
by which types it hands you.** You never have to ask "can I view these bytes?"
— the type you are given already answers it.

### 1.3 Completeness before performance (the interop guarantee)

For the storage formats that hold a customer's existing data — **Parquet, ORC,
Avro** — the top-ranked, non-negotiable requirement is **complete, bit-exact
interop, both directions**: *any valid file a reference writer (Spark, Hive,
Impala, Arrow, the Avro tools, across versions) has ever produced must be
readable, and any file we write must be readable by that same ecosystem.* A
customer adopting Cajeta with terabytes of Parquet cannot hit a single file we
fail to decode, nor emit a file their existing Spark jobs reject. Completeness
**outranks performance**: "95% of your data is readable, and fast" is an adoption
failure, not a win.

Completeness is a long tail — encodings, compression codecs, logical types,
nested/repeated columns, schema evolution accumulated over a decade. **We own the
implementation; we do not link a third-party reference reader** (no Arrow C++,
liborc, etc.). This is the path DuckDB, Polars, and cuDF all take — their own
Parquet/ORC readers, no fat dependency. The completeness guarantee is therefore
carried by **our implementation + a conformance corpus + fail-loud behavior**,
not by a bound backstop:

- **Implement the format completely** — the readers target the full spec, not a
  happy-path subset.
- **Fail loud, never silent.** A feature we have not yet implemented raises a
  clear `UnsupportedFeatureException` naming the encoding/type/version — never a
  silent miscode or partial read. "Can't read it yet" is a tracked defect with a
  precise message; "read it wrong" is forbidden.
- **A conformance corpus is the gate** (§7.1). Every storage-format reader/writer
  is validated bit-exact / round-trip against the official suites
  (`parquet-testing`, ORC, Avro) plus real-world files, cross-checked **offline**
  against the reference tools (we run them to produce golden outputs; we do not
  link them).

Coverage expands release over release; a file in the gap fails loudly and becomes
the next unit of work. The small, stable wire formats (CSV, JSON, Protobuf, Ion)
carry far less completeness risk and are built the same way.

### 1.4 Packaging — core stdlib vs the `cajeta-codec` library

The codec surface splits by **ubiquity** — everything is our own Cajeta code, so
the split is about what every program needs versus what is specialized:

| Lives in | What | Why |
|---|---|---|
| **cajeta-two core stdlib** | `view` (language) · `cajeta.codec.json` · `cajeta.codec.csv` · the `Encoder`/`SchemaEncoder`/`StreamingEncoder` interfaces (`cajeta.wire`) · the staged-access convention | Everyone uses JSON and CSV; both are pure-Cajeta SIMD. The interfaces are the contracts `@Encoding` resolves against, so they are core. |
| **`cajeta-codec` (standalone, importable)** | `cajeta.codec.{protobuf, ion, avro, parquet, orc}` · the columnar tier (`XFile`/`ColumnVector`) · the compression codecs (`Decompressor`/`Compressor`) | Specialized formats most programs never touch. Opt-in: code that never reads Parquet never pulls in the columnar/compression machinery. All our own code — no third-party libraries. |

`cajeta-codec` is a standalone repo on the cajeta-unit / cajeta-logging / cazo
model (own `cajeta.json`, `src/`, `docs/`, `plan/`), imported via classpath
bitcode linking. It depends on the core interfaces and implements them for the
specialized formats. The framework convention (this spec) is language-wide and
stays in core; the per-format specs for the library formats live in the
`cajeta-codec` repo's `docs/`.

---

## 2. The staged-access convention

The bytes flow through stages; each stage is a **named type whose methods are the
only legal next steps**. Holding a given type tells you exactly where you are.

```
raw bytes (int8[] / Buffer)
   │
   ├─ Class A ─────────────────────────────► view X          (zero-copy, terminal)
   │
   ├─ Class B tagged ─► XIndex ─► XCursor ──► values / view   (lazy, skip-capable)
   │                 └► X.parse<T> ─────────► T               (eager materialize)
   │
   ├─ Class B untagged ─► XReader ──────────► T (in order)    (linear)
   │
   └─ Class C ─► XFile ─► [Decompressor] ─► [encoding decode] ─► ColumnVector<T>
                                                              (per-column, on read)
```

### 2.1 Naming rules (the convention)

| Type shape | Means | Next step |
|---|---|---|
| `view X` / `X` (view kind) | zero-copy overlay, fixed layout, **now** | read/write fields directly (`Views.md`) |
| `XIndex` | SIMD structural index over self-describing bytes | hand to `XCursor`, or drive a typed walk |
| `XCursor` | lazy field access positioned over an index | navigate / read / **skip** fields |
| `XReader` | pull/linear reader; bytes are *not* random-access | `next()` / read in order |
| `XSax` + `XHandler` | push (event) parser | implement handler callbacks |
| `X.parse<T>(bytes)` / `Encoder<T>` | eager full materialization into a `T` | use the `T` |
| `XFile` | columnar container; file bytes are **not** addressable | `schema()`, `rowGroups()`, `column<T>(...)` |
| `ColumnVector<T>` | a decoded column, SIMD-ready | iterate / compute / overlay a `view` |
| `Decompressor` | the block-decompress stage | `decompress(src) → bytes` |

The discipline: **a type never lets you skip a mandatory stage.** There is no
`column<T>` on raw file bytes; there is no field accessor on an `XIndex`; a
`view` only constructs over bytes that are already fixed-layout. Illegal
shortcuts are simply un-typeable.

### 2.2 Format facades advertise supported modes

A format's entry facade exposes exactly the access modes its structure permits:

- **Protobuf, Ion** (tagged): `index(bytes)`, `cursor(bytes)`, `parse<T>(bytes)`.
- **JSON** (tagged, shipped): `parse<T>`, `reader`, `sax`, `cursor`, value tree.
- **CSV** (tagged via header): `index(bytes)`, typed row reader, `parse<T>` row bind.
- **Avro** (untagged): `reader(bytes, schema)`, `parse<T>(bytes, schema)` — **no**
  `index`/`cursor`; the absent methods *are* the documentation that random access
  is impossible.
- **Parquet, ORC** (columnar): `XFile` only — never a raw `view` or `parse<T>`
  over the file; you pull columns/row-groups.

---

## 3. Abstraction tiers

Five tiers, ordered raw → typed. The per-format libraries implement these; user
code programs against them.

### 3.1 Tier 0 — `view` (terminal, zero-copy)

Fully specified in `docs/specification/lang/Views.md`. A `view` overlays a typed
layout on a borrowed (or owned) byte buffer; per-field access is a GEP + load.
**Views are valid only over fixed-layout bytes** (Class A) — raw bytes of a
fixed protocol, *or a decoded leaf/column produced by a higher tier* (§5). A
view is never constructed over un-decoded Class B/C bytes; the type system
forbids it by the offset = f(type) requirement.

### 3.2 Tier 1 — streaming readers (lazy, self-describing)

For Class B. Three shapes, mirroring what `cajeta.codec.json` already ships:

- **`XIndex`** — the SIMD structural index. Stage-1 scan that records token /
  tag / delimiter boundaries *without decoding values*. Built once, drives
  everything else.
- **`XCursor`** — lazy random access positioned over an `XIndex`. Decodes a
  field on demand; **skips** unrequested fields and subtrees with no value
  decode (the lever that made `Json.parse<T>` fast).
- **`XReader` / `XSax`+`XHandler`** — the pull and push variants for callers who
  want streaming rather than random access.

### 3.3 Tier 2 — eager binders (`Encoder<T>` family)

Whole-message materialization: bytes ↔ a fully-constructed `T`. The shipped
interface (`runtime/src/cajeta/wire/Encoder.cajeta`):

```cajeta
package cajeta.wire;
public interface Encoder<T> {
    #int8[] encode(T value);     // T -> owned bytes
    #T      decode(int8[] bytes); // owned bytes -> T
}
```

`Encoder<T>` is intentionally thin and stateless — correct for tagged formats
that carry their own structure (Protobuf, MessagePack). Two extensions, both
**proposed**:

- **`SchemaEncoder<T>`** — carries a schema. *Required* for untagged formats
  (Avro), whose bytes are undecodable without one. `Encoder<T>` has no schema
  slot; this is the gap.

  ```cajeta
  public interface SchemaEncoder<T> {
      #int8[] encode(T value, Schema schema);
      #T      decode(int8[] bytes, Schema schema);
  }
  ```

- **`StreamingEncoder<T>`** — incremental encode/decode over a reader/writer
  rather than buffer-to-buffer. Already named as deferred in `Annotations.md` §3.
  Lands without disturbing `Encoder<T>`.

### 3.4 Tier 3 — columnar file readers (`XFile` + `ColumnVector<T>`)

For Class C. A columnar file is **not** an `Encoder<T>` — there is no `T` that is
"a Parquet file." It is a container you open and pull from:

```cajeta
// shape (proposed), illustrative
public class ParquetFile {
    public Schema       schema();
    public RowGroup[]   rowGroups();
    public ColumnVector<T> column<T>(String name);          // decode-on-read
    public ColumnVector<T> column<T>(int rowGroup, String name);
    // projection + predicate pushdown live here, not in Encoder<T>
}
```

`ColumnVector<T>` is the decoded column — a flat, fixed-width, SIMD-ready buffer.
It is the natural place a `view` / typed-array overlay re-enters (§5). The file
reader internally chains `Decompressor` (§3.5) then the encoding-decode kernels;
the caller sees only "ask for a column, get a vector."

The write side is the mirror — `XFileWriter` appends `ColumnVector<T>`s (or row
batches), and internally chains our SIMD *encode* kernels then `Compressor`
(§3.5), emitting spec-conformant output validated by the conformance corpus
(§7.1):

```cajeta
// shape (proposed), illustrative
public class ParquetFileWriter {
    public ParquetFileWriter(Path path, Schema schema);
    public void appendColumn<T>(String name, ColumnVector<T> data);
    public void closeRowGroup();
    public void close();                                  // finalize footer
}
```

This tier owns the analytics-facing concepts `Encoder<T>` cannot express:
**column projection** (read only the columns you need), **predicate pushdown**
(skip row-groups by min/max stats), and **row-group/stripe streaming**.

#### Ownership model

The rule is the existing view borrow model (`Views.md`) extended to codecs:
**every materialized buffer is owned by exactly one variable; everything
downstream borrows it.** The columnar pipeline has **two owned roots**, because
decode produces new bytes that did not exist in the file:

1. **Raw file bytes** — owned by the caller's variable. The `XFile` reader
   **borrows** them (borrow-form construction); the reader cannot outlive the
   buffer. This is exactly "read returns an owned buffer, the API borrows from
   there on."
2. **The decoded `ColumnVector<T>`** — decode (decompress + encoding-expand)
   allocates a fresh buffer, so the column **owns** its backing. `view` overlays
   and iteration over the column then **borrow** the vector (§5). The buffer is
   ours (we allocated it during decode), so ownership is plain — no foreign
   buffer to adopt, no external release hook.

Both `XFile` and column access offer the borrow / owning forms via the `#`
discriminator, as views do: `ParquetFile(bytes)` borrows the file buffer;
`ParquetFile(#bytes)` takes ownership for parse-and-discard flows.

### 3.5 Tier — `Decompressor` (block codec, cross-cutting)

The block-decompress stage as an explicit, reusable component shared by Parquet,
ORC, and Avro container files:

```cajeta
// shape (proposed)
public interface Decompressor {
    #int8[] decompress(int8[] src, int expandedLen);
}
public interface Compressor {
    #int8[] compress(int8[] src);
}
```

Implementations (`Snappy`, `Lz4`, `Zstd`, `Gzip`/`zlib`, `Brotli`) are **our own
code** — see §7.3. `Snappy`/`Lz4` are simple enough to own outright and bit-exact;
`Zstd`/`zlib` are larger lifts whose own-vs-system-lib status is an open decision
(§10). The interface is deliberately separate from `Encoder<T>` because
compression is byte→byte, format-agnostic, and reused across every Class C format.

---

## 4. `@Encoding` — review and repositioning

`@Encoding(EncoderClass)` (`Annotations.md` § Section 3) binds a type to an
`Encoder<T>` so the compiler synthesizes `T(int8[])` (decode) and `toBytes()`
(encode). It is mutually exclusive with `@BigEndian`/`@LittleEndian`/`@Align`.

### 4.1 What it actually is

Despite the spec calling the synthesized constructor a "view constructor,"
`@Encoding` is **eager whole-message materialization**, not a view. The spec
itself says it "does NOT change the in-memory representation… bytes are
materialized only at the encode/decode boundary" and copies decoded fields into a
normal class. It is the Tier-2 binder with annotation sugar. **First correction:
stop describing it as a view** — a view is zero-copy overlay (Tier 0); `@Encoding`
is the opposite (full decode to an object).

### 4.2 Its correct niche, and where it is the wrong tool

| Format | `@Encoding`/`Encoder<T>` fit |
|---|---|
| Protobuf, MessagePack | **Good** — eager whole-message bind. (But also wants the lazy index+cursor mode for skip/projection — §3.2.) |
| Avro | **Insufficient** — needs a schema; use `SchemaEncoder<T>` (§3.3). |
| Parquet, ORC | **Wrong shape** — columnar; no `T`-for-a-file; needs `XFile` (§3.4). |
| CSV | **Borderline** — `Encoder<Row>` per row, but really wants a streaming reader + header schema. |
| JSON | **Excluded by design** — field-name mapping varies per class; that is per-field annotation territory (`@JsonProperty` …) on `Json.parse<T>`, not a class-level encoder. |

### 4.3 Verdict

Keep `@Encoding`/`Encoder<T>` as the **eager-binding tier**. Do not oversell it
as "covers all binary formats." Three changes:

1. Reframe its docs: eager decode, not a view.
2. Add the `SchemaEncoder<T>` / `StreamingEncoder<T>` interface family (§3.3).
3. Leave columnar and lazy access to their own tiers (§3.2, §3.4); `@Encoding`
   does not reach there.

---

## 5. View re-entry — where decoded bytes become viewable again

The intuition "after expansion, a view can access the data" is correct **at the
leaf**, not at the message. Protobuf/Ion decode into object graphs — there is no
fixed buffer to overlay. But the **terminal decoded artifacts are fixed-layout**:

- A decoded Parquet/ORC **column** (after decompress + RLE/dict/bitpack/delta
  expansion) is a flat fixed-width buffer → `ColumnVector<T>` is exactly a typed
  view over it.
- A decoded fixed-width **array leaf** in any format is viewable.

So `view` is both the **entry** mechanism for Class A *and* the **exit**
mechanism at the decoded leaf of Classes B/C. It never applies to the
intermediate wire bytes. This is the single rule that keeps the zero-copy story
coherent: *views overlay fixed-layout bytes — whether they arrived fixed-layout
or were decoded into fixed-layout.*

---

## 6. SIMD / GPU acceleration policy

Drawn from `agents/plans/research/compression/SIMD-ParallelCompression-Analysis.md`
and the SIMD research. The governing rule (owner's): *if SIMD prevents
multi-fiber parallelism or is slower, it is not a win; if it boosts multi-fiber
or beats a 32-core scalar baseline, apply it.*

| Codec layer | Policy | Why |
|---|---|---|
| Structural scan (tag/varint/delimiter index) — Class B | **APPLY SIMD** | stage-1 scan vectorizes; proven in JSON (2–3.8× Jackson); reused for Protobuf/Ion/CSV |
| Integer/columnar encodings (bit-pack, FOR, dict, RLE, delta) — Class C | **APPLY SIMD — biggest win** | ~0.7 cyc/int, >4 G ints/s, sometimes faster than `memcpy`; delta = SIMD prefix-sum (research in hand) |
| Block decompression (LZ: snappy/lz4/zstd/gzip) | **OWN, do not SIMD the chain** | LZ match-copy is an irreducible serial chain ("poor match to SIMT" — nvCOMP); often DRAM-bandwidth-bound. Implemented as our own bit-exact codecs; optimize with branch reduction + wide copies, not vector lanes (§7.3) |
| Cross-page / column / row-group parallelism | **FIBERS** | embarrassingly parallel; near-linear to 32 fibers; the "SIMD inside a fiber, fibers across blocks" structure Cajeta uniquely expresses |
| GPU decode | **BUILD, gated** | throughput-only (SIMT-hostile to LZ); behind a size threshold + "data is going to the GPU anyway" signal (cuDF model) |
| Native (non-interop) columnar store | **BUILD — interleaved-rANS + transforms** | the only place a better-than-zstd codec is legal; rANS interleaving gives SIMD lanes *and* composes with fibers |

---

## 7. Correctness without a bound reader

We implement Parquet/ORC/Avro ourselves (§1.3) — no linked reference reader.
Completeness is guaranteed by three disciplines, not a backstop.

### 7.1 Conformance corpus (the gate)

The guarantee is only real if it is tested. Every storage-format reader/writer is
gated against the official suites (`parquet-testing`, ORC, Avro) plus real-world
files, with assertions:

- **Read:** our decoded output equals the expected values — golden outputs
  produced **offline** by the reference tools (Spark/Arrow/orc-tools/avro-tools)
  and committed as fixtures. We *run* those tools to generate fixtures; we do not
  *link* them.
- **Round-trip (write):** a file we write, read back by us, equals the source;
  and — checked offline in CI — a file we write is accepted by the reference
  reader, and a reference-written file is read by us to the same values.

A file we mis-decode is a hard failure. A file we cannot yet read fails loud
(§7.2) and is a tracked completeness defect, not a silent gap.

### 7.2 Fail-loud, never silent

A format feature we have not implemented raises `UnsupportedFeatureException`
naming the exact encoding / logical type / format version. There is **no silent
fallback and no partial read** — the two failure modes are "read correctly" or
"refuse loudly with a precise message." This is what makes incremental coverage
safe: a customer hitting an unsupported corner gets a clear, actionable error and
a tracked defect, never corrupt data.

### 7.3 Compression — the build seam

For Class C, the decode pipeline has two byte-transform layers, both ours:

```
page bytes → [block-decompress: snappy/lz4/zstd/gzip]  ← OWN (serial chain, no SIMD)
           → encoded column data
           → [encoding-decode: dict/RLE/bit-pack/FOR/delta]  ← OWN (SIMD + fibers)
           → values → ColumnVector<T>
```

**Block decompressor (LZ).** Implemented as our own bit-exact codecs. Reading
files written by Spark/Arrow/DuckDB **requires** byte-exact snappy/lz4/zstd/gzip,
so these are interop-pinned, not a place to be clever. We do not SIMD the LZ
match-copy chain (it is serial and bandwidth-bound); we optimize it with branch
reduction and wide copies. `Snappy`/`Lz4` are small and owned outright;
`Zstd`/`zlib` are larger — own vs. link-a-system-lib is open (§10).

**Encoding-decode layer.** Ours, SIMD + fibers. It touches every value, is the
larger share of columnar decode time, and is the biggest, most reliable SIMD win
(§6).

**A better-than-zstd codec is only legal off the interop path** — a Cajeta-native
columnar store (§6, last row) where both ends are ours.

---

## 8. Per-format application

Each subsection: structure, class, supported access modes, pipeline, SIMD
targets, and **enumerated use cases**. Detailed per-format specs land under
`docs/specification/codec/<format>/` (core) or the `cajeta-codec` repo's `docs/`
(library formats) as each is built.

### 8.1 CSV — `cajeta.codec.csv` (Class B tagged-via-header; **build, SIMD**)

Delimited text, row-oriented. Smallest surface, big SIMD win, structurally
identical to JSON's stage-1 scan (find delimiters / newlines / quotes;
RFC-4180 quoting + escaping).

- **Access:** `CsvIndex` (SIMD structural scan), typed row reader, `Csv.parse<T>`
  row bind via header→field mapping.
- **SIMD:** delimiter/quote classification (the JSON pattern); bulk numeric
  field parse.
- **Use cases:**
  - UC-CSV-1 Stream a multi-GB CSV row-by-row without materializing the file.
  - UC-CSV-2 Bind each row to a typed `T` by header name.
  - UC-CSV-3 Project a subset of columns by header, skipping the rest.
  - UC-CSV-4 Handle RFC-4180 quoting, embedded delimiters/newlines, escapes.
  - UC-CSV-5 Write rows from typed values with correct quoting.

### 8.2 Protobuf — `cajeta.codec.protobuf` (Class B tagged; **build, SIMD**)

Tag-length-value with varint encoding, row-oriented. Tractable to decode; varint
boundary scanning is SIMD-able (Stream VByte / Masked VByte family). Fits the
compile-time synthesizer: `.proto` → Cajeta types, or annotation-described
messages.

- **Access:** `ProtobufIndex`, `ProtobufCursor` (lazy field skip/projection),
  `Protobuf.parse<T>` eager, plus `@Encoding(ProtobufEncoder<T>.class)`.
- **SIMD:** varint/tag boundary scan (stage-1 index); bulk packed-repeated
  primitive fields.
- **Use cases:**
  - UC-PB-1 Decode a message into a typed `T`.
  - UC-PB-2 Lazily read two fields of a large message, skipping the rest.
  - UC-PB-3 Decode `packed` repeated numeric fields via SIMD.
  - UC-PB-4 Round-trip encode a `T` to wire bytes (`decode(encode(v)) == v`).
  - UC-PB-5 Generate Cajeta message types from a `.proto` schema *(open: §10)*.

### 8.3 Ion — `cajeta.codec.ion` (Class B tagged; **build, SIMD**)

Amazon Ion binary: self-describing, typed, with a leading symbol table. Same
tagged pipeline as Protobuf. Ion's text form is a JSON superset and could reuse
the JSON scanner; **binary Ion is the priority.**

- **Access:** `IonIndex`, `IonCursor`, `Ion.parse<T>`; symbol-table resolution at
  index time.
- **SIMD:** typed-value boundary scan; symbol-table-driven field skip.
- **Use cases:**
  - UC-ION-1 Decode binary Ion into a typed `T`, resolving the symbol table.
  - UC-ION-2 Lazily access fields, skipping unrequested subtrees.
  - UC-ION-3 Preserve Ion's richer type set (timestamps, decimals, blobs).
  - UC-ION-4 Round-trip encode `T` → binary Ion.
  - UC-ION-5 *(stretch)* Parse Ion text via the JSON scanner path.

### 8.4 Avro — `cajeta.codec.avro` (Class B **untagged**; **build, schema-driven**)

Schema-driven, untagged, row-oriented; container files carry the schema (JSON)
and a block codec in the header. Undecodable without the schema → linear reader
only, no random access. Modest SIMD (bulk fixed-width blocks; zigzag-varint
scalars do not vectorize well).

- **Access:** `AvroReader(bytes, schema)`, `Avro.parse<T>(bytes, schema)` via
  `SchemaEncoder<T>` (§3.3); container-file header parse (schema + codec).
- **Pipeline:** header (schema + codec) → block → our `Decompressor` → linear
  decode.
- **Use cases:**
  - UC-AVRO-1 Read an Avro Object Container File: parse header schema + codec.
  - UC-AVRO-2 Decode records in schema order into typed `T`.
  - UC-AVRO-3 Decompress deflate/snappy/zstd blocks via our `Decompressor`.
  - UC-AVRO-4 Resolve reader-vs-writer schema differences *(open: §10)*.
  - UC-AVRO-5 Encode `T` → Avro under a given schema.

### 8.5 Parquet — `cajeta.codec.parquet` (Class C columnar; **build — all our own code**)

Columnar storage: Thrift-serialized footer (schema, row-group/column offsets,
stats), column chunks of pages, lightweight encodings then page compression.
Large surface — phased (§9); completeness by conformance corpus + fail-loud (§7).

- **Access:** `ParquetFile` → `schema()`, `rowGroups()`, `column<T>()` →
  `ColumnVector<T>`; projection + predicate pushdown.
- **Pipeline:** footer (Thrift) → page locate → our `Decompressor` →
  encoding-decode (SIMD: dict/RLE/bit-pack/delta) → `ColumnVector<T>`.
- **SIMD:** the encoding-decode kernels (§7.3); delta = prefix-sum. **Fibers**
  across pages/columns/row-groups. GPU gated (§6).
- **Use cases:**
  - UC-PARQ-1 Open a file, read its schema and row-group metadata.
  - UC-PARQ-2 Read a single column across all row-groups into a `ColumnVector<T>`.
  - UC-PARQ-3 Project two of fifty columns, touching only their chunks.
  - UC-PARQ-4 Predicate pushdown: skip row-groups by min/max stats.
  - UC-PARQ-5 Decode dictionary + RLE + bit-packed + delta columns via SIMD.
  - UC-PARQ-6 Decompress snappy/zstd/gzip pages via our `Decompressor`.
  - UC-PARQ-7 Decode many pages/columns concurrently across fibers.
  - UC-PARQ-8 Overlay a `view` / typed array on a decoded fixed-width column (§5).

### 8.6 ORC — `cajeta.codec.orc` (Class C columnar; **build — all our own code**)

Columnar storage: protobuf-serialized footer/metadata, stripes of row-groups,
typed streams, RLE v1/v2 + dictionary, stripe-level compression. Same shape as
Parquet (reuses the columnar tier, the `Decompressor`, and most encoding
kernels); differs in metadata (protobuf vs Thrift), stripe structure, and ORC's
RLE v2 variants.

- **Access:** `OrcFile` → `schema()`, `stripes()`, `column<T>()` →
  `ColumnVector<T>`; projection + predicate pushdown.
- **Pipeline:** footer (protobuf) → stripe → stream locate → our `Decompressor`
  → encoding-decode (SIMD: RLE v1/v2, dict) → `ColumnVector<T>`.
- **SIMD / fibers / GPU:** as Parquet (§8.5).
- **Use cases:**
  - UC-ORC-1 Open a file, read its protobuf footer + schema.
  - UC-ORC-2 Read a column across stripes into a `ColumnVector<T>`.
  - UC-ORC-3 Project a subset of columns by name.
  - UC-ORC-4 Predicate pushdown via stripe/row-group stats.
  - UC-ORC-5 Decode RLE v1/v2 + dictionary streams via SIMD.
  - UC-ORC-6 Decompress zlib/snappy/zstd streams via our `Decompressor`.
  - UC-ORC-7 Decode stripes/columns concurrently across fibers.

---

## 9. Phasing

Dependency order (the plan sequences against this). **Core stdlib:** steps 1–2.
**`cajeta-codec` library:** steps 3 onward.

1. **Foundation** *(core stdlib)* — the staged-access convention, the tier
   interfaces (`SchemaEncoder<T>`, `StreamingEncoder<T>`, `Decompressor`/
   `Compressor`, `XFile`/`ColumnVector<T>` shapes), and the `@Encoding` doc
   repositioning. No format work; just the vocabulary every format depends on.
2. **CSV** *(core stdlib)* — smallest Class B; read + write; validates the
   convention end-to-end and reuses the JSON stage-1 SIMD scan.
3. **Protobuf** *(lib)* — Class B tagged binary; read + write + the `@Encoding`
   eager path + lazy cursor; establishes varint SIMD.
4. **Ion** *(lib)* — second tagged binary; reuses Protobuf's index/cursor
   machinery.
5. **Compression codecs** *(lib)* — our own snappy/lz4 (+ zstd/gzip, own vs.
   system-lib per §10); prerequisite for Avro/Parquet/ORC.
6. **Avro** *(lib)* — first schema-driven untagged; read + write; exercises
   `SchemaEncoder<T>` + the `Decompressor` tier on a row format.
7. **Columnar tier** *(lib)* — `XFile`/`XFileWriter`/`ColumnVector<T>` + the
   shared SIMD encode/decode kernels (bit-pack/FOR/dict/RLE/delta) + fiber
   orchestration.
8. **Parquet** *(lib)* — full columnar reader + writer on the tier (Thrift footer).
9. **ORC** *(lib)* — reader + writer reusing the tier (protobuf footer, RLE v2).
10. **GPU decode** *(lib)* — gated columnar offload, once CPU kernels are proven.
11. **Native columnar store** *(lib, optional)* — interleaved-rANS + transforms
    off the interop path.

Writers are in v1 alongside their readers (read + write decided), not a deferred
trailing phase. The conformance corpus + fail-loud disciplines (§7) gate every
storage-format step.

---

## 10. Decisions and open questions

### Decided

- **Implementation strategy** — **our own code, no third-party libraries.**
  Completeness guaranteed by our implementation + conformance corpus + fail-loud,
  not a bound reference reader (§1.3, §7).
- **Packaging** — `cajeta.codec.{json, csv}` in core stdlib; `cajeta.codec.{
  protobuf, ion, avro, parquet, orc}` + columnar tier + compression in the
  standalone `cajeta-codec` library (§1.4).
- **Schema source** — **annotation-described messages first**; `.proto`/`.avsc`
  importers (codegen) are a later, separate tooling track.
- **`ColumnVector<T>` ownership** — file bytes owned by the caller and borrowed by
  the reader; the decoded column owns the buffer we allocate during decode; access
  borrows the column. Borrow/own forms via `#` (§3.4).
- **Read + write in v1** — both directions for every format; writers ship
  alongside readers (§7, §9).

### Open

- **Compression-codec ownership** — `Snappy`/`Lz4` owned outright; `Zstd`/`zlib`
  own-implement (bit-exact, large lift) vs. link a ubiquitous system library.
- **Reader-vs-writer schema resolution** (Avro, and Parquet/ORC schema
  evolution): how much of the formats' schema-migration rules to support in v1.
- **Predicate pushdown surface** — expression model for row-group skipping; how
  far beyond min/max stats (bloom filters, dictionaries) in v1.
- **Nested/repeated types** — Parquet definition/repetition levels and ORC nested
  types; v1 flat-and-simple-nesting vs full nesting (unsupported → fail-loud).
- **Write-side control surface** — how much encoder selection / statistics /
  compression-codec choice we expose on the write path.

---

## 11. What this is not

- **Not a universal overlay.** No single mechanism spans all formats; the
  framework selects the mechanism, it does not unify them (§1).
- **Not dependent on third-party libraries.** Every codec is our own Cajeta code;
  completeness is carried by conformance corpus + fail-loud, not a bound reader
  (§7).
- **Not a replacement for `view`.** `view` is Tier 0 — fixed-layout overlay; the
  framework routes everything else and hands control back to `view` at decoded
  leaves (§5).
- **Not value-semantics serialization sugar.** `@Encoding` is eager codec
  binding, not a layout annotation (§4).
