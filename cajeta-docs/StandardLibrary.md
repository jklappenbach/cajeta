# StandardLibrary.md

A design for cajeta's standard library — what packages exist, what types live in
each, and the shape of their public surface. This document captures intent;
implementation lands incrementally as separate `.cajeta` files under
`./runtime/src/`.

## Goals

- Cover the everyday primitives a server / data-processing program needs:
  strings, time, collections, error types, basic IO.
- Make the `Collection` hierarchy uniform — including arrays — so generic code
  doesn't need to special-case primitive-shaped containers.
- Surface clear performance choices (sparse vs dense, ordered vs hashed,
  growable vs immutable) instead of one "Map" / one "Set" that hides the trade-off.
- Encoding-aware strings: a `String` holds character content, not bytes;
  conversion to/from bytes is explicit and parameterized by encoding.
- Pay-for-what-you-use: nothing in the stdlib should require linking machinery
  the user didn't ask for. Time-zone tables, regex, formatter caches — all
  lazy.

## Non-goals (v1)

- Locale-aware collation, normalization (Unicode NFC/NFD), full ICU.
- Reflection / runtime annotation introspection beyond what AspectModel.md
  already specifies.
- A regex engine in this pass — defer to a later library.
- Async primitives beyond `Thread.sleep` / `Fiber.sleep` — full reactor lives
  in cajeta-docs/ThreadModel.md.

## Package layout

```
cajeta.error           — Throwable hierarchy (already shipping inline; move to disk)
cajeta.lang            — String, Integer, Long, Double, Boolean, Object,
                         Encoding enum, primitive boxing as needed
cajeta.time            — Instant, Duration, Period, LocalDate, LocalTime,
                         LocalDateTime, ZoneId, ZoneOffset, ZonedDateTime,
                         DateTimeFormatter, Clock (with nanoTime / millisTime)
cajeta.collection      — Collection / Iterable / Iterator interfaces;
                         List, Set, Map, Deque, Stack interfaces;
                         Sortable / ArraySortable interfaces — sequence
                         types that sort in place; the ArraySortable
                         half adds user-pluggable SortAlgorithm support
                         (Array-backed types only);
                         StructView marker for zero-alloc views over
                         external buffers;
                         Array (the variable-size, element-typed array
                         — heap-resident, replaces T[] for non-inline use);
                         ArrayList, LinkedList;
                         HashSet, TreeSet, DenseSet, SparseSet;
                         HashMap, TreeMap, DenseMap, SparseMap, IdentityHashMap;
                         ArrayDeque, LinkedDeque, ArrayStack;
                         Heap (priority queue); BitSet;
                         tree.{BinaryTree, RedBlackTree, BTree, BPlusTree};
                         sort.{quick, merge, timsort, heap, insertion,
                         radix, counting, parallel variants, external};
                         Immutable[List, Set, Map, Deque, Array, Heap] —
                         read-only variants
cajeta.io              — InputStream, OutputStream, Reader, Writer, byte buffers;
                         the linked-list-of-buffers shape used by network code
cajeta.net             — Socket, ServerSocket (later — needs the reactor)
cajeta.thread          — Thread, Fiber, Sleep, Mutex (extends what
                         ThreadModel.md already documents)
cajeta.math            — boxed Object equivalents for every native numeric
                         type (including the fp4 / fp6 / fp8 variants),
                         RoundingMode, precision-aware casting, BigInteger,
                         BigDecimal, Rational, Math intrinsics; Random +
                         SecureRandom; Guid32 / Guid64 / Guid128. See
                         CajetaML.md "Prerequisite: cajeta.math expansion"
                         for the full surface.
cajeta.hash            — Hasher interface; XXHash3 (fast general-purpose,
                         the DefaultHasher backing); RapidHash (maximum
                         throughput when ecosystem interop isn't needed);
                         SipHash (DoS-resistant, key-derived); MD5
                         (checksum / identifier, NOT for security);
                         Hash utility namespace (identity, combine,
                         seed); DefaultHasher used by the compiler-
                         synthesized Object.hash(). Cryptographic hashes
                         (SHA-2, SHA-3, BLAKE2/3) and AEAD / signature /
                         KDF primitives live in the future cajeta.crypto
                         peer library.
```

---

## cajeta.lang

### `String`

Immutable, encoding-aware character sequence. Internal storage: UTF-8 byte
array plus a cached code-point count. A `String` carries a tagged mode
internally — **owned** (heap-allocated; `new String(...)`) or **view**
(borrowed over bytes that live elsewhere; `String.viewOf(...)`). See
"Strings over struct byte fields" below for the view path.

```cajeta
public final class String implements Collection<int32>,
                                      StructView<String, byte[]> {
    // Owned construction — `new` always allocates.
    public String();
    public String(byte[] bytes, Encoding encoding);
    public static String fromCodePoints(int32[] codePoints);
    public static String repeat(String s, int64 n);

    // View construction — borrows from `source`. No allocation, no
    // memcpy. Borrow checker ties the result's lifetime to source.
    public static String viewOf(byte[N]& source, Encoding encoding = Encoding.UTF_8);
    public static String viewOf(byte[]& source, int64 byteCount,
                                 Encoding encoding = Encoding.UTF_8);
    public static String cString(byte[N]& source, Encoding encoding = Encoding.UTF_8);

    // Promote view → owned (allocates + memcpys).
    public String toOwned();

    // Inspection (both modes)
    public int64 count();                          // code-point count
    public int64 byteCount();                      // raw byte count
    public boolean isEmpty();
    public int32 codePointAt(int64 index);         // O(index) on UTF-8
    public boolean equals(String other);
    public int32 compare(String other);
    public int64 hash();

    // Search (both modes)
    public int64 indexOf(String needle);
    public int64 indexOf(String needle, int64 fromIndex);
    public int64 lastIndexOf(String needle);
    public boolean contains(String needle);
    public boolean startsWith(String prefix);
    public boolean endsWith(String suffix);

    // Transformation. Always returns a fresh OWNED String — these can't
    // mutate the source bytes in view mode. `substring` of a view that
    // stays within the source returns a sub-view (cheap, no alloc);
    // crossing-boundary substrings promote to owned.
    public String substring(int64 start, int64 endExclusive);
    public String concat(String other);
    public String replace(String from, String to);
    public String toUpperCase();
    public String toLowerCase();
    public String trim();
    public String[] split(String separator);
    public String[] lines();

    // Encoding round-trip
    public byte[] getBytes(Encoding encoding);

    // Iteration — yields code points in order (both modes)
    public Iterator<int32> iterator();
}
```

`String` implements `Collection<int32>` so `for (cp in someString) { ... }`
works and so `count()` is consistent across the rest of the collection
hierarchy. Iteration is over code points, not bytes — bytes are accessible via
`getBytes(Encoding.UTF_8)` when needed.

#### Strings over struct byte fields

Structs don't have `String` fields. A struct's job is to be an exact
byte layout that overlays cleanly onto a buffer; an embedded heap
`String` pointer would break that contract, and the earlier "inline
variable-size `String` field" proposal required compiler magic plus a
"must be last" / "at most one" rule that ate field-ordering freedom.
Both are gone.

The replacement: struct fields are plain bytes (`byte[N]` for
fixed-width, `byte[?]` for a variable tail). At access time, code
constructs a `String` **view** over the field — no allocation, no
memcpy, the view borrows the struct's bytes for as long as the
struct is alive.

**The model.** A `String` carries a tagged internal representation:
**owned** (heap-allocated, the existing path) or **view** (a borrow
over bytes that live somewhere else — another struct, an array, a
buffer). Read-only operations work the same on both modes;
transformations (concat, replace, toUpperCase, substring crossing a
boundary) always produce a fresh **owned** String.

Construct a view via the `String.viewOf(...)` factory — never `new`.
The convention across stdlib: **`new` always allocates; `viewOf` /
`cString` / similar factories borrow.** Reading the call site tells
you whether the heap got hit. The factory signatures are part of the
canonical `String` declaration above; the rest of this section shows
how the field-reference shape interacts with them.

**The compiler does the address math.** When you write `s.name` in
`String.viewOf(s.name)`, the field reference carries three pieces:
`s`'s base address, the compile-time `offsetof(MyStruct, name)`, and
the buffer size from `byte[N]`'s `N`. The view constructor receives a
fat pointer for free — no length argument, no manual offset
arithmetic, no `&` operator at the user level.

**The fixed-width case.** Every field is plain bytes; Strings are
constructed where needed:

```cajeta
struct PacketHeader {
    int32 magic;
    byte[16] name;          // 16 bytes inline, no length prefix
    int32 sequence;
    byte[16] tag;
}

PacketHeader p = PacketHeader(buf);

// Zero-copy view over the 16 inline bytes. No allocation.
String n = String.viewOf(p.name);

// Equivalent — explicit encoding for a non-UTF-8 wire format.
String n = String.viewOf(p.name, Encoding.ASCII);

// Null-padded (C-string style) — scans up to the first null.
String n = String.cString(p.name);

// Use n freely within p's scope. The borrow checker rejects code
// that stores n into a longer-lived field, returns it past p's
// scope, or transfers it to another fiber:
String persisted = n.toOwned();   // explicit materialization
```

Multiple String fields are fine — no "must be last," no "at most
one." Each is just `byte[N]` from the struct's perspective; `String`
construction is a per-call-site decision.

**The variable-length case.** When the content varies per message,
the struct uses `byte[?]` (the existing variable-tail mechanism in
the compiler) plus an explicit length field. The `String` view is
constructed exactly the same way — `viewOf(source, byteCount)` over
the variable tail:

```cajeta
struct LogMessage {
    int64 timestamp;
    int32 severity;
    int32 bodyLength;       // explicit, addressable, no magic
    byte[?] body;           // variable-tail byte array
}
```

Layout in memory:

```
+----------------+----------------+----------------+----------------+
| timestamp (8)  | severity (4)   | bodyLength (4) | body bytes...  |
+----------------+----------------+----------------+----------------+
^                                                  ^                ^
struct base                                        end of fixed     end of buffer
                                                    footprint        (length bytes)
```

Read and write:

```cajeta
LogMessage m = LogMessage(buf);   // overlay onto byte[] buf

// Zero-copy view over the variable tail. No allocation.
String body = String.viewOf(m.body, m.bodyLength);

// Use body freely; persist via .toOwned() if needed.
String persisted = body.toOwned();

// Writing back goes through the underlying byte[] field, not
// through `String` (a view is read-only). For a wire-format
// builder use Buffer or write the bytes directly:
buf.writeBytesAt(offsetof(LogMessage, body), newContent);
m.bodyLength = newContent.length;
```

The rule that remains is on the **byte[?]** terminal-variable field
itself (it must be last; the offsets of subsequent fields would be
runtime-dependent otherwise). That rule is about raw variable-tail
storage, not about `String` — structs with multiple `String` *views*
work because each view is constructed over a `byte[N]` field, and
`byte[N]` is fixed-size.

#### Where owned and view Strings meet

Both modes are the same type, so user code mostly doesn't notice.
The differences show up at the boundaries:

- **Allocation.** `new String(...)` allocates and copies. `String.
  viewOf(...)` doesn't. The call site tells you.
- **Lifetime.** A view's lifetime is tied to the source it borrows
  from. The borrow checker rejects escapes; `.toOwned()` is the
  documented escape hatch when persistence is needed.
- **Mutating operations.** Concatenation, replace, toUpperCase, etc.
  always return a fresh **owned** String — they can't mutate the
  underlying bytes (the source might not even be writable). A
  view's `substring(start, end)` can return a sub-view when the
  result stays within the source (cheap, no alloc); pass the
  result through `.toOwned()` if it needs to escape.
- **IDE display.** Hover on a view-mode `String` value shows
  `String (view of <source>)`; an owned String shows `String
  (owned)`. The doc comments on the factories say "Returns a
  String view over `source`. No allocation. Borrow checker ties
  the result's lifetime to source."

### `Encoding`

Enum with the encodings cajeta supports natively. UTF-8 is the canonical
internal form; conversions to/from others go through `getBytes` and the
`String(byte[], Encoding)` constructor.

```cajeta
public enum Encoding {
    UTF_8,
    UTF_16_LE,
    UTF_16_BE,
    UTF_32_LE,
    UTF_32_BE,
    ASCII,
    ISO_8859_1,
    WINDOWS_1252,
}
```

Operations that can't represent a code point in the target encoding throw
`EncodingException` (a `RecoverableException` subtype). Lossy modes
("replace with `?`", "skip") arrive later via an optional `Encoder` strategy
class — out of scope for v1.

### Boxed primitives — `Integer`, `Long`, `Double`, `Boolean`

Thin wrappers that exist so primitives can be stored in `Collection<T>` and
related generic types. Cajeta's primitive types (`int32`, `int64`, `float32`,
`float64`, `boolean`) stay separate; boxing happens at the boundary.

The static parse/format methods (`Integer.parseInt`, `Double.valueOf`, etc.)
are already intrinsic in MethodCallExpression's namespace dispatcher.
StandardLibrary.md just documents that they exist; their canonical
declarations land in `cajeta.lang.Integer` etc.

### `Object`

The universal root of the class hierarchy. Every class implicitly extends
Object. Four methods, all with compiler-synthesized structural defaults so
the common case requires no boilerplate:

```cajeta
public class Object {
    public boolean operator==(Object obj);
    public int64 hash();
    public String toString(Encoding e = UTF_8);
    public Object clone();
}
```

**Default implementations are structural, not identity-based.** Compiler
synthesizes:

- `operator==(Object obj)`: instanceof-check against the declaring class, cast,
  field-by-field comparison. The Java pattern "always implement equals with
  null-check, instanceof, cast, compare" — written for you by the compiler.
- `hash()`: combines the same field set the structural `operator==` consults,
  using `cajeta.hash.DefaultHasher` (xxHash3-based, see `cajeta.hash`). The
  compiler emits one `Hasher.writeX(...)` call per field, finishes with
  `Hasher.finish()`, and inlines the whole thing when the field set is known
  at compile time — no allocation, just register-level multiply-rotate-xor
  ops per field. A per-process random seed (initialized from OS entropy at
  startup) is mixed in, so hash values are stable for the value's lifetime
  in this process but not stable across process restarts. The seed kills
  hash-flooding attacks: an attacker can't predict bucket placement to
  force pathological O(n²) HashMap behavior.
- `toString(Encoding)`: `TypeName(field1=value1, field2=value2, ...)`. The
  Rust `#[derive(Debug)]` shape, sufficient for `println(x)` debug output.
  Encoding parameter defaults to UTF-8; other encodings supported via the
  default-argument mechanism cajeta already has.
- `clone()`: field-by-field copy. For value-typed fields (primitives,
  structs, enums) the copy is a memcpy of the bits; for class-typed
  fields the copy is a shallow reference copy by default (both originals
  point at the same heap instance — same semantics as Java's
  `Object.clone()` shallow path). Override `clone()` manually to deep-
  copy class-typed fields when that's the right thing. The return type
  is `Object` in the base; the compiler narrows it to the declaring
  class at every override site so call-site code doesn't cast.

**Override pair enforcement.** If a class declares `operator==` manually, it
must also declare `hash()` (and vice versa). The compiler refuses to compile
a class with one but not the other. The contract — equal values hash equally —
is structurally protected by requiring both halves to be authored together.
`toString` has no pair requirement; override it independently.

**Universal participation in hash-based collections.** Any class — yours,
third-party, stdlib — works as a `HashMap<K, V>` key or `HashSet<T>` element
without a `derives` annotation, an `implements` clause, or any other opt-in.
The synthesized defaults are real, useful implementations. No `Hashable` or
`Equatable` interface exists; the methods live directly on Object.

**`Comparable<T>` stays as an opt-in interface** because natural ordering is
domain-specific — there's no sensible compiler default for "compare a class
with an int field and a String field." `TreeMap<K, V>` and `TreeSet<T>` carry
`K extends Comparable<K>` as a bound; HashMap and HashSet do not.

**Cyclic-type detection at compile time.** When the compiler synthesizes
`hash()` / `operator==` / `toString()` it walks the class's field type graph.
If any field type can transitively reach back to the class itself, the
naive synthesized walk would recurse forever, so the compiler refuses to
emit the defaults and produces a diagnostic naming the offending field:

```
error: can't synthesize hash() / operator==() / toString() on `tree.Node`:
       field `parent` (type `tree.Node`) creates a cycle through self.
       Mark a field along the cycle as @transient, or implement these
       methods manually.
```

User fixes:
- **`@transient` field annotation** — the synthesizer skips the annotated
  field when walking fields for hash/equals/toString. Java borrowed
  `transient` from for-serialization-skip; cajeta repurposes it for
  "compiler shouldn't traverse this when synthesizing." Cheap fix for the
  common case (one back-reference closes the cycle).
- **Manual implementation** — for complex shapes (mutual recursion across
  N classes, diamonds, memoizing traversal). User implements both `hash()`
  and `operator==` (the pair requirement still applies).

The cycle analysis also runs through generic instantiations
(`LinkedList<Node>` where `Node` references `LinkedList<Node>` is a cycle).

**Identity hash as a separate intrinsic.** The rare case that genuinely wants
pointer-based hashing — observer maps, graph node identity, weak-reference
keys — goes through `cajeta.hash.Hash.identity(obj)` as a runtime intrinsic,
and `IdentityHashMap<K, V>` ships as a separate type that uses it internally.
No
`Hashable` bound on K because IdentityHashMap doesn't ask K to hash itself.

---

## cajeta.time

Modeled on `java.time` (JSR-310), pared down for cajeta's needs. Top-level
clock methods live on a `Clock` class; the rest are value types.

### `Clock`

Static utilities for "what time is it":

```cajeta
public final class Clock {
    // Monotonic, ns precision, for interval measurement. Backed by
    // CLOCK_MONOTONIC on Linux; not adjustable by NTP / leap seconds.
    public static int64 nanoTime();

    // Wall clock, ms precision. Backed by CLOCK_REALTIME. Can jump
    // backward if the system clock is adjusted.
    public static int64 millisTime();

    // Current instant from the system clock.
    public static Instant now();
}
```

### Value types

All immutable, comparable, hashable.

- **`Instant`** — a moment on the UTC timeline, ns precision. Internally:
  `int64 secondsSinceEpoch + int32 nanos`.
  ```cajeta
  public final class Instant {
      public static Instant now();
      public static Instant ofEpochSecond(int64 seconds);
      public static Instant ofEpochMilli(int64 millis);
      public int64 epochSecond();
      public int32 nano();
      public Instant plus(Duration d);
      public Instant minus(Duration d);
      public Duration between(Instant other);
      public ZonedDateTime atZone(ZoneId zone);
  }
  ```

- **`Duration`** — a time-based amount, ns precision. Negative durations OK.
  Methods: `ofMillis`, `ofSeconds`, `ofMinutes`, `ofHours`, `ofDays`, `plus`,
  `minus`, `multipliedBy`, `negated`, `toMillis`, `toNanos`, comparison.

- **`Period`** — a calendar-based amount: years, months, days. Distinct from
  `Duration` because months aren't fixed-length. Used with `LocalDate`.

- **`LocalDate`** — `yyyy-mm-dd`, no time, no zone. `of(year, month, day)`,
  `plusDays`, `plusMonths`, `plusYears`, `dayOfWeek`, `dayOfYear`, `isLeapYear`.

- **`LocalTime`** — `hh:mm:ss.nnnnnnnnn`, no date, no zone. Constructors,
  arithmetic, conversion.

- **`LocalDateTime`** — `LocalDate` + `LocalTime`. The common "what time was
  this event" type that doesn't yet know a zone.

- **`ZoneId`** — a time-zone identifier (`America/Los_Angeles`,
  `Europe/London`, `UTC`). Resolves to offsets via the system tz database.
  Loaded lazily; the database isn't pulled in unless you actually call
  `ZoneId.of("...")`.

- **`ZoneOffset`** — a fixed offset from UTC (`+05:30`, `-08:00`). Subtype of
  `ZoneId` for zones that don't observe DST.

- **`ZonedDateTime`** — `LocalDateTime` + `ZoneId`. The "fully resolved point
  in human time at a place" type. Round-trippable to/from `Instant`.

- **`DateTimeFormatter`** — pattern-based formatting and parsing. Patterns
  follow the ISO-8601 / `java.time.format` convention (`yyyy-MM-dd'T'HH:mm:ssXXX`).
  Pre-defined constants for `ISO_INSTANT`, `ISO_LOCAL_DATE`,
  `ISO_LOCAL_DATE_TIME`, `ISO_ZONED_DATE_TIME`, `RFC_1123_DATE_TIME`.

### Open questions

- **Time zone database.** Linux ships `/usr/share/zoneinfo`. Cajeta could
  parse those files at first use, or bake a compact representation into the
  runtime. The first is simpler; the second is portable. Recommended:
  read-from-disk with a fallback embedded for UTC + fixed offsets so a static
  build keeps working on systems without tzdata.
- **Leap seconds.** `Instant` ignores them (UTC-SLS convention, same as
  java.time). Document explicitly.
- **Nanosecond precision everywhere.** Java's `Instant` is ns precision but
  many platforms only deliver ms. We'd document that the system clock
  resolution may be coarser than the type's precision.

---

## cajeta.collection

The reorganization point you flagged. Single hierarchy, every container
implements a clear contract, arrays included.

### Top of the hierarchy

```cajeta
public interface Iterable<T> {
    public Iterator<T> iterator();
}

public interface Iterator<T> {
    public boolean hasNext();
    public T next();
}

public interface Collection<T> extends Iterable<T> {
    public int64 count();
}
```

`Iterable` is separated so types that only need "give me an iterator" don't
inherit `count()` they'd have to implement. Streams, generators, file lines
— those will be `Iterable` but not `Collection`.

### `StructView` — zero-alloc views over external storage

Some collections can be constructed as a *view* over bytes that live
elsewhere (a struct field, an existing array, a buffer slice) — no
allocation, no memcpy, just a borrow of the source's memory.
`StructView` marks the collection types that support this. The
contract is a pair of static factories whose names start with
`viewOf`; the convention across stdlib is that **`new` allocates,
`viewOf` borrows**.

```cajeta
public interface StructView<Self, Source> {
    // Construct an instance whose internal storage borrows from
    // `source`. No allocation, no memcpy. The borrow checker ties
    // the returned instance's lifetime to `source`.
    public static Self viewOf(Source& source);

    // Same, when the source's length isn't known at the type
    // level (a runtime-sized byte[] tail in a struct, for example).
    public static Self viewOf(Source& source, int64 length);
}
```

**View-mode collections are read-only.** Mutating operations
(`add`, `set`, `push`, `pop`, `sort` in-place) are rejected at
compile time on view-mode instances — calling them prints a
diagnostic pointing at `.toOwned()`:

```
error: cannot call `Heap<int32>.push` on a view-mode instance.
       The heap views `source` (declared at line 14) and views
       are read-only. Call `.toOwned()` to materialize a heap-
       allocated, mutable copy.
```

Read-only operations (`count`, `iterator`, `peek`, `contains`,
indexing, `equals`) work uniformly on owned and view modes — no
branch on the caller's side.

**The view's lifetime is tied to the source.** Returning a view
past its source's scope, storing it in a longer-lived field, or
transferring it to another fiber is a compile-time error.
`.toOwned()` materializes a fresh heap-allocated, fully-owned
instance — that crosses any boundary.

**Which collections implement `StructView`:**

| Collection         | Implements? | Storage shape                                                       |
|--------------------|-------------|--------------------------------------------------------------------|
| `Array<T>`         | yes         | Contiguous bytes of `T`. The canonical case.                       |
| `String`           | yes         | UTF-8 bytes (other encodings supported).                           |
| `BitSet`           | yes         | Packed bits over a backing word array.                             |
| `Heap<T>`          | yes         | Binary heap stored as a contiguous array.                          |
| `ArrayStack<T>`    | yes         | Same backing as Array.                                             |
| `ArrayList<T>`     | yes         | View has no capacity slack — append rejected; reads fine.          |
| `ArrayDeque<T>`    | no          | Head/tail offsets + wrap-around can't be inferred from raw bytes.  |
| `LinkedList<T>`    | no          | Nodes scattered in memory.                                         |
| `HashMap` / `HashSet` | no       | Hash table + chains, scattered.                                    |
| `TreeMap` / `TreeSet` | no       | Pointer-linked tree.                                               |
| `BinaryTree<T>`    | no          | Pointer-linked.                                                    |
| `BTree` / `BPlusTree` | no       | Pointer-linked between pages.                                      |

The "no" entries don't have a contiguous representation to borrow
into. If you need to ship a `LinkedList<int32>` over the wire,
convert to `Array<int32>` first (`list.toArray()`); the array can
then participate in the view-construction story end-to-end.

### `Array<T>` implements `Collection<T>`, `StructView<Array<T>, byte[]>`

The load-bearing piece of "arrays included in Collection polymorphism,"
and the canonical `StructView` implementor.

- `int32[]`, `byte[]`, `Foo[]`, etc. all carry a length in their header
  (already true today — `__cajeta_new_array` does this). `count()` reads that
  length.
- The compiler synthesizes an `iterator()` for any array type that yields
  elements in index order.
- Code that takes `Collection<int32>` accepts an `int32[]` directly. No
  wrapping.

**View construction.** The `viewOf` factory takes a byte buffer (any
contiguous source) and produces a read-only Array of the element type
viewed over those bytes — no allocation, no memcpy.

```cajeta
public final class Array<T> implements Collection<T>, StructView<Array<T>, byte[]> {
    // Owned — `new` always allocates.
    public Array(int64 length);

    // View over a typed slice of an external byte buffer. Element
    // count comes from `byteCount / sizeof(T)`. Read-only.
    public static Array<T> viewOf(byte[]& source, int64 byteCount);

    // View over a fixed-size byte buffer (N comes from the type).
    public static Array<T> viewOf(byte[N]& source);

    public T at(int64 index);                 // both modes
    public void set(int64 index, T value);    // owned only
    public int64 count();                     // both modes
    public Array<T> toOwned();                // promote view → owned
}
```

Wire-format usage — overlay a typed array directly over the buffer
that came off the socket:

```cajeta
struct ProbeBatch {
    int32 batchId;
    int32 sampleCount;
    byte[?] samples;        // sampleCount * sizeof(int32) bytes
}

ProbeBatch b = ProbeBatch(buf);

// Zero-copy view over the samples region as int32[].
Array<int32> samples = Array<int32>.viewOf(b.samples,
                                            b.sampleCount * 4);

int32 total = 0;
for (sample in samples) total += sample;     // pure read; works on view

// Promote when we need to keep them past `b`'s scope:
Array<int32> archived = samples.toOwned();
```

Open question: does Array's interface participation require runtime magic
(compiler treats `T[]` as having `Collection<T>` methods) or does it work
through normal implements-interface resolution? Recommended: compiler-level
— the array struct has a vtable slot that points at a generated
`count`/`iterator` pair for the element type, so `(Collection<T>) myArr`
dispatches normally.

### `List<T>` and concrete lists

```cajeta
public interface List<T> extends Collection<T> {
    public T get(int64 index);
    public void set(int64 index, T value);
    public void add(T value);
    public void addAt(int64 index, T value);
    public T removeAt(int64 index);
    public int64 indexOf(T value);
}
```

Concrete types:
- **`ArrayList<T>`** — growable array, amortized O(1) append. Implements
  `StructView<ArrayList<T>, byte[]>`; views are read-only (`add` rejected
  because there's no capacity slack to grow into).
- **`LinkedList<T>`** — doubly-linked, O(1) insert/remove at known position,
  O(1) at-ends. Also implements `Deque<T>`. **Not** `StructView` — nodes
  are scattered.

```cajeta
// ArrayList view over wire bytes — read-only, zero-alloc.
ArrayList<int64> ids = ArrayList<int64>.viewOf(batch.idsField,
                                                batch.idCount * 8);
for (id in ids) process(id);
// ids.add(99);   // compile error: cannot mutate a view
ArrayList<int64> kept = ids.toOwned();   // for storage past batch's scope
```

### `Set<T>` and concrete sets

```cajeta
public interface Set<T> extends Collection<T> {
    public boolean contains(T value);
    public boolean add(T value);     // returns true if added
    public boolean remove(T value);
}
```

Concrete types — split along storage policy:
- **`HashSet<T>`** — separate-chaining hash table. General-purpose default.
- **`DenseSet<T>`** — open-addressing (linear probing). Better cache behavior
  for small T; slightly higher memory.
- **`TreeSet<T>`** — ordered, red-black tree. `T` must be `Comparable<T>`.
  Adds `first()`, `last()`, `floor(T)`, `ceiling(T)`, range queries.
- **`BitSet`** — dense set of `int32` keys, one bit per possible value.
  Specialized; the right answer for "set of ints in a known small range."
  Implements `Set<int32>` and `StructView<BitSet, byte[]>` — a view over
  a packed bit field in an existing buffer:

  ```cajeta
  // Feature-flag bitmap viewed straight out of a config payload.
  BitSet flags = BitSet.viewOf(payload.flagBytes, /*bitCount=*/256);
  if (flags.contains(FLAG_FAST_PATH)) ...
  // flags.add(99);   // compile error: view is read-only
  ```

### `Map<K, V>`

```cajeta
public interface Map<K, V> extends Collection<Entry<K, V>> {
    public V get(K key);
    public V put(K key, V value);     // returns previous value or null
    public V remove(K key);
    public boolean containsKey(K key);
    public Set<K> keys();
    public Collection<V> values();
}

public class Entry<K, V> {
    public K key();
    public V value();
}
```

Concrete types — same storage-policy split as sets:
- **`HashMap<K, V>`** — separate-chaining hash table. Default.
- **`DenseMap<K, V>`** — open-addressing (linear probing).
- **`SparseMap<K, V>`** — for cases where most keys would map to a default
  value. Memory-efficient when the keyspace is large but populated sparsely.
  Implementation note: a HashMap with a default-value fallback semantically;
  the "sparse" framing is about API ergonomics, not a different algorithm.
- **`TreeMap<K, V>`** — ordered by key.
- **`IdentityHashMap<K, V>`** — keys compared by reference identity, not
  by `operator==` / `hash()`. Use when the key's structural identity
  doesn't model "same key" (graph node maps, observer registries,
  weak-ref tables). The map calls `cajeta.hash.Hash.identity(key)` for
  bucket selection and pointer equality for slot lookup, so `K` is
  not required to override `hash()` — it works on any class
  regardless of what its structural hash returns.

  ```cajeta
  // Tracking visited nodes during graph traversal — even two nodes
  // with identical contents are distinct entries.
  IdentityHashMap<GraphNode, VisitState> seen = new IdentityHashMap();
  for (node in graph.nodes()) {
      if (!seen.containsKey(node)) {
          seen.put(node, VisitState.NEW);
          visit(node);
      }
  }
  ```

Open question: do `SparseMap` and `HashMap` actually need to be separate
types, or is `SparseMap` just `HashMap` with a `withDefault(V)` builder
method? Recommended: same type, builder method. Keeps the hierarchy smaller.

### `Deque<T>` and `Stack<T>`

```cajeta
public interface Deque<T> extends Collection<T> {
    public void pushFront(T value);
    public void pushBack(T value);
    public T popFront();
    public T popBack();
    public T peekFront();
    public T peekBack();
}

public interface Stack<T> extends Collection<T> {
    public void push(T value);
    public T pop();
    public T peek();
}
```

Concrete:
- **`ArrayDeque<T>`** — ring-buffer-backed Deque. Default. **Not**
  `StructView` — head/tail offsets + wrap-around can't be inferred
  from raw bytes.
- **`LinkedDeque<T>`** — alias for `LinkedList<T>` (which already qualifies).
  **Not** `StructView` — nodes are scattered.
- **`ArrayStack<T>`** — Stack on top of ArrayList. Implements
  `StructView<ArrayStack<T>, byte[]>`; views are read-only (`peek` and
  iteration work, `push` / `pop` rejected).

```cajeta
// ArrayStack view over a wire-format frame's payload region.
ArrayStack<int32> incoming = ArrayStack<int32>.viewOf(frame.payload,
                                                      frame.count * 4);
int32 top = incoming.peek();      // read-only ops work
// incoming.pop();   // compile error: cannot mutate a view
```

### `Heap<T>` — priority queue

The binary-heap-backed priority queue, missing from earlier drafts.
Backing storage is a single contiguous `Array<T>` with the standard
heap-order invariant (parent at index `i`, children at `2i+1` and
`2i+2`); operations are O(log n) push / pop, O(1) peek, O(n) heapify.

```cajeta
public final class Heap<T> implements Collection<T>, StructView<Heap<T>, Array<T>> {
    // Owned — `new` always allocates. Default ordering uses
    // `Comparable<T>`; supply a comparator to override.
    public Heap();
    public Heap(Comparator<T> cmp);
    public Heap(Array<T> items);              // O(n) heapify

    public static Heap<T> minHeap();
    public static Heap<T> maxHeap();

    // View — read-only over an already heap-ordered array. The
    // comparator must match what produced the ordering (callers
    // either supply it or accept natural ordering).
    public static Heap<T> viewOf(Array<T>& source);
    public static Heap<T> viewOf(Array<T>& source, Comparator<T> cmp);

    public void push(T value);                // owned only
    public T    pop();                        // owned only
    public T    peek();                       // both modes
    public int64 count();                     // both modes
    public boolean isEmpty();                 // both modes

    public Iterator<T> drainSorted();         // owned only — destructive
    public Iterator<T> iterator();            // both — heap order, NOT sorted

    public Heap<T> toOwned();                 // promote view → owned
}
```

Usage — owned, building up a priority queue:

```cajeta
Heap<int64> nextAt = Heap<int64>.minHeap();
nextAt.push(timestamp + 1000);
nextAt.push(timestamp + 250);
int64 due = nextAt.peek();        // 250-from-now wins
nextAt.pop();
```

Usage — view, consuming a wire-format frame that already holds a
heap-ordered batch (e.g. an upstream-prioritized work queue):

```cajeta
struct PriorityBatch {
    int32 jobCount;
    byte[?] jobIds;       // jobCount * 8 bytes, already heap-ordered
}

PriorityBatch p = PriorityBatch(buf);
Array<int64> jobs = Array<int64>.viewOf(p.jobIds, p.jobCount * 8);
Heap<int64> priorityQ = Heap<int64>.viewOf(jobs);

int64 next = priorityQ.peek();    // O(1), zero allocation
// priorityQ.push(99);  // compile error: view is read-only
```

`Heap` is the only `StructView` implementor in the stdlib whose
storage layout encodes a non-trivial invariant (the heap order).
Callers that construct a heap view over arbitrary unsorted bytes
get well-formed peek/iteration but undefined `pop`-like semantics
— the contract is "the source is already heap-ordered." Views
exist for shipping a heap over the wire (sender heapifies once,
receiver consumes via a view), not for converting an arbitrary
array into a heap zero-cost.

### Sorting

In `cajeta.collection.sort`. Two layers: a `Sort` namespace of
static algorithms that operate on `Array<T>&` directly (the
fast path, no abstraction overhead), and the `Sortable<T>` /
`ArraySortable<T>` interfaces that collections implement to
expose in-place sort through their own API surface. The
algorithms are the substance; the interfaces are how user code
reaches them through `myList.sort(...)`.

```cajeta
public final class Sort {
    // ----- comparison-based, single-threaded -----

    // General-purpose. Introsort: quicksort with median-of-three
    // pivot + insertion-sort fallback for ranges under 32 +
    // heapsort fallback when quicksort depth exceeds 2*log2(n).
    // Unstable, in-place, O(n log n) worst case.
    public static <T extends Comparable<T>> void quick(Array<T>& data);
    public static <T> void quick(Array<T>& data, Comparator<T> cmp);

    // Stable mergesort. O(n log n) worst case, O(n) extra space.
    // Pick when equal-element relative order matters.
    public static <T extends Comparable<T>> void merge(Array<T>& data);
    public static <T> void merge(Array<T>& data, Comparator<T> cmp);

    // Stable, adaptive. Timsort — exploits existing runs in the
    // input (common in real-world data); same O(n log n) worst case
    // but O(n) on already-sorted or reverse-sorted input. Python /
    // Java default.
    public static <T extends Comparable<T>> void timsort(Array<T>& data);
    public static <T> void timsort(Array<T>& data, Comparator<T> cmp);

    // Worst-case-guaranteed O(n log n), in-place, unstable. Heapsort.
    // Slower than quicksort on average but with no bad-input
    // pathologies — pick when worst-case timing matters more than
    // common case (real-time systems, adversarial inputs).
    public static <T extends Comparable<T>> void heap(Array<T>& data);
    public static <T> void heap(Array<T>& data, Comparator<T> cmp);

    // Adaptive O(n²). Fast on small / nearly-sorted data; used as
    // the base case inside the other sorts for short ranges. Public
    // because it's occasionally the right standalone answer.
    public static <T extends Comparable<T>> void insertion(Array<T>& data);
    public static <T> void insertion(Array<T>& data, Comparator<T> cmp);

    // Partial sort — sorts only the first k elements, in O(n log k).
    // Useful for "top k" queries without sorting the rest.
    public static <T extends Comparable<T>> void partial(Array<T>& data, int64 k);

    // ----- linear-time sorts for specific domains -----

    // Counting sort. O(n + range). Only useful when the value range
    // is small (caller supplies the min/max bound).
    public static void countingInt32(Array<int32>& data, int32 min, int32 max);
    public static void countingInt64(Array<int64>& data, int64 min, int64 max);

    // Radix sort (LSB, byte-at-a-time). O(n · digits) — for fixed-
    // width integer and float keys, beats comparison sorts on large
    // arrays once n is past ~10K. Stable. Allocates O(n) scratch.
    public static void radix(Array<int8>& data);
    public static void radix(Array<int16>& data);
    public static void radix(Array<int32>& data);
    public static void radix(Array<int64>& data);
    public static void radix(Array<uint8>& data);
    public static void radix(Array<uint16>& data);
    public static void radix(Array<uint32>& data);
    public static void radix(Array<uint64>& data);
    public static void radix(Array<float32>& data);    // bit-pattern-aware
    public static void radix(Array<float64>& data);

    // Radix sort over arbitrary keys via key-extraction. The key
    // type must be one of the supported fixed-width numeric types.
    public static <T, K> void radix(Array<T>& data, Function<T, K> keyOf);

    // ----- fiber-parallel variants -----

    // Parallel introsort. Splits the input recursively; each
    // partition past a per-fiber threshold (~32K elements) spawns
    // a new fiber. Below threshold, single-thread fallback. Useful
    // when comparison is non-trivial (custom comparator does
    // string work, etc.) — comparison sorts scale well when the
    // comparator is the bottleneck.
    public static <T extends Comparable<T>> void quickParallel(
        Array<T>& data,
        int8 fiberCount = -1);     // -1 = OS CPU count
    public static <T> void quickParallel(
        Array<T>& data,
        Comparator<T> cmp,
        int8 fiberCount = -1);

    // Parallel mergesort. Splits the input into fiberCount chunks,
    // sorts each on its own fiber, parallel-merges pairs in O(log
    // fiberCount) rounds. Stable. Useful for large datasets where
    // memory bandwidth isn't yet the binding constraint.
    public static <T extends Comparable<T>> void mergeParallel(
        Array<T>& data,
        int8 fiberCount = -1);
    public static <T> void mergeParallel(
        Array<T>& data,
        Comparator<T> cmp,
        int8 fiberCount = -1);

    // Parallel radix sort — see "Parallel radix sort" subsection
    // below for the full description. Fiber-friendly LSB radix
    // with cache-line-aware histogram partitioning, lock-free
    // scatter. The fastest way to sort tens of millions of fixed-
    // width keys on a multi-core CPU.
    public static void radixParallel(Array<int32>& data, int8 fiberCount = -1);
    public static void radixParallel(Array<int64>& data, int8 fiberCount = -1);
    public static void radixParallel(Array<uint32>& data, int8 fiberCount = -1);
    public static void radixParallel(Array<uint64>& data, int8 fiberCount = -1);
    public static void radixParallel(Array<float32>& data, int8 fiberCount = -1);
    public static void radixParallel(Array<float64>& data, int8 fiberCount = -1);
    public static <T, K> void radixParallel(
        Array<T>& data,
        Function<T, K> keyOf,
        int8 fiberCount = -1);

    // ----- external sort (data doesn't fit in memory) -----

    // Streams `input` into chunks of `chunkBytes`, sorts each chunk
    // in memory, writes to `workspace`, then merges. Multi-way merge
    // (typically 16-way) keeps disk I/O sequential. Returns the
    // path of the sorted output.
    public static Path external(Path input, Path workspace,
                                 int64 chunkBytes,
                                 Comparator<byte[]> cmp);
}
```

**How sorts attach to collections.** Three layered entry points:

**1. Static algorithms on `Array<T>` — the canonical mutable
contiguous storage.** Every `Sort.X(...)` static method takes
`Array<T>&` and sorts in place. This is the bare-metal API: pick
the algorithm explicitly, no indirection.

```cajeta
Array<int32> readings = collectReadings();
Sort.radix(readings);                          // in-place
Sort.quick(readings, byTimestampAsc);          // with comparator
```

**2. `Sortable<T>` / `ArraySortable<T>` interfaces — collections
that can sort in place.** Two interfaces, deliberately split.
`Sortable<T>` is the minimum contract: "I'm a sequence you can
sort." `ArraySortable<T>` adds "you can pick which algorithm" —
which requires random-access contiguous storage, so it's only
implemented by array-backed types. The split exists because
some algorithms (quicksort, radix, counting, the parallel
variants) need contiguous memory and random-access indexing to
work efficiently; forcing those onto LinkedList would mean
silently allocating a temporary array under the user's `sort()`
call, which violates the "no hidden allocation" rule the
stdlib design follows everywhere else.

```cajeta
// Minimum sort contract — every sortable collection has at least this.
// The implementation picks an algorithm that suits its storage shape
// (introsort on Array-backed types, linked-list mergesort on LinkedList).
public interface Sortable<T> {
    public void sort();                           // requires T extends Comparable<T>
    public void sort(Comparator<T> cmp);
}

// Algorithm-selection contract — only collections with random-access
// contiguous storage. Adds the SortAlgorithm-taking overloads.
public interface ArraySortable<T> extends Sortable<T> {
    public void sort(SortAlgorithm<T> alg);
    public void sort(SortAlgorithm<T> alg, Comparator<T> cmp);
}

// Algorithms are values, not enum constants — users can write their
// own and pass them where stdlib's would go. The algorithm contract
// operates on Array<T>& for raw performance (no virtual dispatch per
// element access; cache-line locality preserved).
public abstract class SortAlgorithm<T> {
    public abstract void sort(Array<T>& data, Comparator<T> cmp);

    // Natural-order shortcut. Algorithms that work on raw bit
    // patterns (radix, counting) override this directly and ignore
    // the comparator path.
    public void sortNatural(Array<T>& data) where T extends Comparable<T> {
        sort(data, NaturalOrder.<T>get());
    }
}

// Stdlib instances live as static factories on Sort:
//   Sort.quickAlgorithm<T>()
//   Sort.mergeAlgorithm<T>()
//   Sort.timsortAlgorithm<T>()
//   Sort.heapAlgorithm<T>()
//   Sort.radixAlgorithm(/* numeric type-specific */)
//   Sort.quickParallelAlgorithm<T>(int8 fiberCount = -1)
//   Sort.mergeParallelAlgorithm<T>(int8 fiberCount = -1)
//   Sort.radixParallelAlgorithm(/* numeric type-specific */, int8 fiberCount = -1)
```

Sorting is only meaningful for collections with **sequence
semantics** — an order the user controls and that an in-place
operation can rearrange. Sets and maps have no inherent
positional order to rearrange (a `HashSet` stores by bucket; a
`TreeSet` is already key-ordered by construction); calling
`sort()` on them would be either nonsensical or a no-op, so
neither interface is implemented on them. The compiler catches
the misuse at the type level — there's no Sortable contract to
call.

| Collection         | `Sortable<T>` | `ArraySortable<T>` | Default sort                                          |
|--------------------|---------------|---------------------|-------------------------------------------------------|
| `Array<T>`         | yes           | yes                 | introsort, plus any user-supplied `SortAlgorithm<T>`  |
| `ArrayList<T>`     | yes           | yes                 | same — delegates to backing Array                     |
| `ArrayStack<T>`    | yes           | yes                 | same                                                  |
| `ArrayDeque<T>`    | yes           | yes                 | same (un-wraps the ring buffer internally)            |
| `LinkedList<T>`    | yes           | **no**              | linked-list mergesort, in-place, no allocation        |
| `HashSet<T>`       | no            | no                  | n/a — no inherent order to rearrange                  |
| `DenseSet<T>`      | no            | no                  | same                                                  |
| `BitSet`           | no            | no                  | always ordered by bit position; sort is a no-op       |
| `TreeSet<T>`       | no            | no                  | already ordered by construction                       |
| `HashMap<K, V>`    | no            | no                  | no inherent order over entries                        |
| `DenseMap<K, V>`   | no            | no                  | same                                                  |
| `SparseMap<K, V>`  | no            | no                  | same                                                  |
| `IdentityHashMap<K, V>` | no       | no                  | same                                                  |
| `TreeMap<K, V>`    | no            | no                  | already ordered by key                                |
| `Heap<T>`          | no            | no                  | sorts via `drainSorted` / `toSortedArray` — see "Heap and sorting" |
| `BinaryTree<T>`    | no            | no                  | tree shape, not sequence                              |
| `RedBlackTree<T>`  | no            | no                  | already ordered                                       |
| `BTree`/`BPlusTree`| no            | no                  | already ordered                                       |

Array-backed implementers forward `sort()` to `Sort.quickAlgorithm`
(introsort default) on their backing array; `sort(alg, cmp)`
forwards to the user-supplied algorithm. View-mode instances
reject any `sort` call at compile time (same rule as every other
mutator — views are read-only).

**LinkedList is `Sortable` but not `ArraySortable`** — and that
distinction is the whole point of the split. Linked-list mergesort
is in-place, O(n log n), needs no random access and no extra
allocation; that's what `LinkedList.sort()` runs. But arbitrary
algorithms aren't supported because they'd require materializing
the linked list into an array first, sorting that, and re-linking
the nodes — a hidden cost the user wouldn't see at the call site.
The type system makes that decision explicit instead:

```cajeta
ArrayList<Event> events = loadEvents();
events.sort(byTimestamp);                            // introsort default
events.sort(Sort.timsortAlgorithm<Event>(), byTimestamp);
events.sort(Sort.radixParallelAlgorithm<Event>(   // pluggable algorithm
    extractTimestamp), byTimestamp);

LinkedList<Node> nodes = buildChain();
nodes.sort(byDepth);                                 // linked-list mergesort
// nodes.sort(Sort.radixAlgorithm<Node>(...));       // compile error:
                                                     //   LinkedList isn't ArraySortable

// If radix on linked-list contents is what you need, the conversion
// is your decision, written at the call site:
Array<Node> arr = nodes.toArray();
arr.sort(Sort.radixAlgorithm<Node>(extractDepth));
// nodes still holds the unsorted linked list; arr is the sorted snapshot.

HashSet<String> tags = readTags();
// tags.sort();                                      // compile error — Set has no Sortable
Array<String> sorted = Sort.collected(tags);         // materialize-and-sort instead
```

**User-defined sorts.** Because `SortAlgorithm<T>` is an abstract
class, not an enum, users can publish their own algorithms and
plug them into the same `ArraySortable.sort(alg, cmp)` entry
point stdlib uses:

```cajeta
public class StoogeSort<T> extends SortAlgorithm<T> {
    public void sort(Array<T>& data, Comparator<T> cmp) {
        stoogeRecurse(data, 0, data.count() - 1, cmp);
    }
}

ArrayList<Event> events = loadEvents();
events.sort(new StoogeSort<Event>(), byTimestamp);
```

The dispatcher in `ArraySortable.sort(alg, cmp)` hands the
backing array to the user's algorithm directly — no copies, no
adapters. Research code, specialized domain sorts (timestamp-
aware, locality-aware), and library-supplied algorithms (a future
`cajeta.ml.sort.bucketed` for ML pipelines, say) all compose with
the same instance method users already know. Linked-list-native
algorithms are out of scope for the `SortAlgorithm<T>` family —
they'd need a separate `LinkedSortAlgorithm<T>` operating on
node pointers — and v1 doesn't ship that; `LinkedList.sort()`'s
built-in mergesort covers the documented use case.

**Heap and sorting.** A `Heap<T>` already satisfies a partial-order
invariant — the root is the minimum (or maximum) by definition.
Heapsort and `drainSorted` are literally the same algorithm:
repeatedly pop the root, sifting down each time; total cost O(n log
n). They differ only in *output staging* — heapsort stashes each
popped element back into the same underlying array (so the array
ends up sorted), while `drainSorted` yields each popped element
through an iterator and lets the storage shrink.

Heap doesn't implement `Sortable` because the interface's contract
doesn't fit: `Sortable.sort()` returns `void` and leaves the
instance a valid example of its declared type. After a heap is
sorted, the storage no longer satisfies the heap invariant —
subsequent `push` / `pop` are on a non-heap and break the
contract. Either `sort()` would have to re-heapify afterward (no-
op visible behavior) or leave a broken heap (silent corruption).

The right operations for heap-to-sorted-output return the result
explicitly, so the consumption is visible at the call site:

```cajeta
// Streaming — yield one element at a time, no full materialization.
Iterator<T> drainSorted();

// Materialize — empty the heap, return a fresh sorted Array.
Array<T> toSortedArray();
```

```cajeta
Heap<Task> work = priorityQ;
for (task in work.drainSorted()) {       // streaming, destructive
    runTask(task);
}
// work is empty here; calling work.peek() throws.

// Or materialize all at once:
Array<Task> orderedBatch = work.toSortedArray();
```

The `Array<T>` / `Iterator<T>` return type makes the heap-becomes-
unusable consequence visible. `Sortable.sort()`'s `void` return
would hide exactly that. Sort.collected(heap) also works (it goes
through Collection<T>'s iterator); pick whichever return shape
the caller wants.

**3. `Sort.collected(Collection<T>&)` — materialize and sort
anything iterable.** When you have a `Collection<T>` and need a
sorted snapshot, this is the entrypoint. Allocates a fresh
`Array<T>`, copies elements in, sorts, returns. The source is
unchanged.

```cajeta
// Convert any collection to a sorted array.
public static <T extends Comparable<T>> Array<T> collected(
    Collection<T>& source);
public static <T> Array<T> collected(
    Collection<T>& source,
    Comparator<T> cmp);
public static <T> Array<T> collected(
    Collection<T>& source,
    SortAlgorithm alg,
    Comparator<T> cmp);
```

```cajeta
HashSet<String> tags = readTags();
Array<String> sortedTags = Sort.collected(tags);   // unique + sorted

Map<UserId, Score> scores = loadScores();
Array<Entry<UserId, Score>> ranked = Sort.collected(scores, byScoreDesc);
```

**`Heap<T>.drainSorted()` for streaming sorted output.** Adjacent
to but distinct from `Sort`: a heap consumed in pop order yields
its elements in sorted order incrementally — useful for k-smallest
/ k-largest queries (don't sort the rest), or for merging N
already-sorted streams (push the heads of each into a heap, pop
the smallest, advance that stream). The `drainSorted` iterator
returns elements one at a time without ever materializing the
fully sorted array; cheaper than `Sort.collected` when the caller
only consumes a prefix.

```cajeta
// k-smallest without sorting the whole input
Heap<int64> topK = Heap<int64>.maxHeap();
for (value in stream) {
    topK.push(value);
    if (topK.count() > k) topK.pop();
}
Array<int64> result = Array<int64>.fromIterator(topK.drainSorted());
```

**Comparison-sort selection guide:**

| Use case                                              | Pick           |
|-------------------------------------------------------|----------------|
| General purpose, don't care about stability           | `Sort.quick`   |
| Stable order required (sorting on multiple keys)      | `Sort.timsort` |
| Worst-case timing matters (real-time, adversarial)    | `Sort.heap`    |
| Small array (≤32) or already-near-sorted              | `Sort.insertion`|
| Top-k only (don't waste work on the rest)             | `Sort.partial` |
| Large array of integer / float keys (n > ~10K)        | `Sort.radix`   |
| Same, multi-core CPU available                        | `Sort.radixParallel` |
| Large array of arbitrary objects, multi-core          | `Sort.quickParallel` or `Sort.mergeParallel` |

#### Parallel radix sort

The fiber-friendly version of LSB radix sort. The contract is the
same — stable, O(n · digits), beats comparison sort once n is large
enough — but the work is split across fibers so wall-clock time
scales near-linearly with CPU count until memory bandwidth saturates.

**Algorithm sketch:**

For each digit pass (default 8 bits / 256 buckets, configurable to
11 bits / 2048 buckets for very large inputs where the wider
buckets reduce pass count):

1. **Partition.** The input array is split into `fiberCount`
   contiguous slices. Each fiber owns one slice.
2. **Local histogram.** Each fiber walks its slice and counts
   per-bucket occurrences into a local 256-entry histogram. No
   inter-fiber communication during this pass; the histograms
   are stack-allocated, cache-line-aligned (8 bytes × 256 +
   padding = ~2KB per fiber, fits in L1).
3. **Histogram barrier.** Fibers synchronize via cajeta.thread
   barrier primitive. After the barrier, all local histograms
   are visible to all fibers.
4. **Prefix-sum reduction.** Each fiber computes its **starting
   offset per bucket in the output array** as:

   ```
   offset[bucket] = sum over earlier fibers' local counts
                  + sum over lower-numbered buckets' total counts
   ```

   This is a single pass over `fiberCount × 256` ints — small
   enough that one fiber can do it, or split among all fibers
   in a parallel prefix sum for very large fiberCount.
5. **Scatter.** Each fiber walks its slice again and writes each
   element to `output[offset[bucket]++]`. Because each fiber's
   per-bucket offset starts at a distinct position computed in
   step 4, no two fibers write to overlapping output ranges, and
   the writes are lock-free.
6. **Swap.** Output buffer becomes input for the next digit pass.

After all digit passes, the array is sorted.

**Cache-line awareness:**

- Local histograms are padded to a multiple of 64 bytes and aligned
  to a cache line, so no two fibers share a cache line during the
  histogram pass.
- Output writes are coalesced: each fiber writes to its own range
  of the output array. Since the ranges are contiguous and
  non-overlapping, false sharing only happens at the boundary
  between two fibers' ranges (one cache line at most). Negligible
  in practice.
- The histogram-barrier step uses cajeta.thread's barrier, which
  yields the fiber if another fiber is still working — keeps the
  scheduler responsive instead of busy-waiting.

**Fiber count selection:**

`fiberCount = -1` (default) means "use the OS CPU count" — the
scheduler is fiber-based, but radix sort is CPU-bound and benefits
from one fiber per hardware thread, no more. Going past the CPU
count adds context-switch overhead without parallelism gain.

For batch processing where multiple radix sorts run concurrently
(several pipelines feeding into a server), set `fiberCount`
explicitly to a fraction of CPU count so the sorts don't fight
each other for cores.

**When radix wins:**

- n > ~10K for `int32` / `float32`.
- n > ~50K for `int64` / `float64` (more digit passes).
- Keys are fixed-width — radix is wrong for variable-length
  comparison (use `Sort.timsort` with a string-comparator instead).
- Keys cluster into a known range — counting sort beats it for
  very small ranges (use `Sort.countingInt32` with explicit bounds).

**Float bit-pattern handling.**

IEEE-754 floats sort correctly by raw bit pattern *if* you flip
the sign bit on positives and flip every bit on negatives — the
implementation does this transparently in a pre-pass and reverses
it after the sort. NaN ordering is unspecified by IEEE-754; the
implementation places NaNs at one end (high) so deterministic.

In a `cajeta.collection.immutable` sub-package:

```cajeta
public final class ImmutableList<T> extends ArrayList<T> { ... }
public final class ImmutableSet<T> extends HashSet<T> { ... }
public final class ImmutableMap<K, V> extends HashMap<K, V> { ... }
public final class ImmutableDeque<T> extends ArrayDeque<T> { ... }
public final class ImmutableArray<T> extends Array<T> { ... }
public final class ImmutableHeap<T> extends Heap<T> { ... }
```

Mutators (`add`, `put`, `remove`, `push`, `pop`, …) throw
`UnsupportedOperationException`. The immutable types have stronger compiler
optimizations available because element identity is stable — no rehash,
no resize, no concurrent-modification checks.

`Immutable*` is distinct from `StructView` mode even when both render the
collection effectively read-only. Immutable instances own their storage
(allocated by the construction call); views borrow storage from
elsewhere. Both reject mutation; the difference is lifetime and
ownership semantics, not the mutation contract.

Alternative design: an `@Immutable` annotation on a collection variable
that the compiler treats as read-only. Cleaner syntactically; harder to
enforce once values escape across method boundaries. Recommended:
separate concrete types, simpler.

### Trees

In `cajeta.collection.tree`. None of these implement `StructView` — all
are pointer-linked, so there's no contiguous representation to borrow.

**`BinaryTree<T>`** — bare binary search tree, no balancing. The
building-block type if you want to write your own algorithms (worst-case
degenerates to O(n) on sorted insertion); rarely the right answer on its
own. Implements `Collection<T>` so it composes into the rest of the
hierarchy.

```cajeta
public final class BinaryTree<T> implements Collection<T> {
    public BinaryTree();
    public BinaryTree(Comparator<T> cmp);

    public boolean add(T value);
    public boolean remove(T value);
    public boolean contains(T value);
    public T       find(T probe);     // returns matching element or null

    public int64   count();
    public int64   height();          // depth of the tree
    public boolean isEmpty();

    // Traversal order is explicit — pick the one that matches the use.
    public Iterator<T> iteratorInOrder();
    public Iterator<T> iteratorPreOrder();
    public Iterator<T> iteratorPostOrder();
    public Iterator<T> iteratorLevelOrder();
    public Iterator<T> iterator();    // defaults to in-order
}
```

**`RedBlackTree<T extends Comparable<T>>`** — self-balancing binary
search tree. Backs `TreeSet` and `TreeMap`; usable directly when you
want ordered-by-value semantics without the Set/Map surface.

```cajeta
public final class RedBlackTree<T extends Comparable<T>> implements Set<T> {
    public RedBlackTree();
    public RedBlackTree(Comparator<T> cmp);

    public boolean add(T value);          // O(log n), rebalances
    public boolean remove(T value);       // O(log n)
    public boolean contains(T value);     // O(log n)

    // Ordered-set navigation
    public T first();
    public T last();
    public T floor(T value);              // largest element ≤ value
    public T ceiling(T value);            // smallest element ≥ value

    // Range queries — O(log n) seek + O(k) walk for k results
    public Iterable<T> range(T low, T high);
    public Iterable<T> headSet(T high);   // elements < high
    public Iterable<T> tailSet(T low);    // elements ≥ low

    public int64 count();
    public Iterator<T> iterator();        // in-order
}
```

**`BTree<K extends Comparable<K>, V>`** — disk-friendly multi-way tree.
Used when the dataset doesn't fit in memory; pages get loaded on
demand and cached. Implements `Map<K, V>`.

```cajeta
public final class BTree<K extends Comparable<K>, V> implements Map<K, V> {
    // In-memory construction. `order` controls fan-out per node;
    // ~64 is a typical sweet spot for an in-memory tree.
    public BTree(int8 order = 64);
    public BTree(int8 order, Comparator<K> cmp);

    // Disk-backed construction. The Storage abstraction (cajeta.io)
    // provides the page-read / page-write hooks.
    public static BTree<K, V> openOnDisk(Path path, int8 order = 64);

    public V    get(K key);                       // O(log_order n)
    public V    put(K key, V value);              // O(log_order n)
    public V    remove(K key);
    public boolean containsKey(K key);

    public Iterable<Entry<K, V>> range(K low, K high);
    public Iterable<Entry<K, V>> entriesInOrder();
    public Set<K> keys();
    public Collection<V> values();

    // Disk-backed lifecycle.
    public void flush();
    public void close();
}
```

**`BPlusTree<K extends Comparable<K>, V>`** — leaf-linked B-tree
variant. All data lives in the leaves; internal nodes are pure index;
leaves form a doubly-linked list so range scans are O(log n) seek
plus O(k) sequential walk with no further tree traversal. The right
pick when the workload is heavy on range scans (time-series, log
indexes, ordered iteration over a slice).

```cajeta
public final class BPlusTree<K extends Comparable<K>, V> implements Map<K, V> {
    public BPlusTree(int8 order = 64);
    public BPlusTree(int8 order, Comparator<K> cmp);
    public static BPlusTree<K, V> openOnDisk(Path path, int8 order = 64);

    public V    get(K key);
    public V    put(K key, V value);
    public V    remove(K key);
    public boolean containsKey(K key);

    // The leaf-link advantage — range scan walks the leaf chain
    // directly, no re-descent per element.
    public Iterable<Entry<K, V>> range(K low, K high);
    public Iterable<Entry<K, V>> entriesInOrder();
    public Iterator<Entry<K, V>> iteratorFrom(K start);

    public Set<K> keys();
    public Collection<V> values();

    public void flush();
    public void close();
}
```

**v1 ship order.** `RedBlackTree` is required (it backs
`TreeSet` / `TreeMap`); `BinaryTree` is trivial so it ships alongside.
`BTree` / `BPlusTree` are deferred until first real user — they bring
the cajeta.io storage-page dependency along, which is its own scope
and shouldn't gate stdlib v1.

---

## cajeta.hash

All hashing — the algorithms `Object.hash()` uses internally, the
public surface user code reaches for content fingerprinting / cache
key derivation / file integrity checks, the identity-hash intrinsic
that `IdentityHashMap` is built on. Cryptographic hashes (SHA-2,
SHA-3, BLAKE2 / BLAKE3) and the surrounding crypto primitives
(AEAD, signatures, KDFs) live in the future **cajeta.crypto** peer
library; `cajeta.hash` is for non-cryptographic and crypto-broken-
but-still-useful (MD5) algorithms.

### The `Hasher` interface

Every hash algorithm implements `Hasher` so the same API works
across single-shot, streaming, and structural cases:

```cajeta
public interface Hasher {
    public Hasher writeBoolean(boolean v);
    public Hasher writeInt8(int8 v);
    public Hasher writeInt16(int16 v);
    public Hasher writeInt32(int32 v);
    public Hasher writeInt64(int64 v);
    public Hasher writeUInt8(uint8 v);
    public Hasher writeUInt16(uint16 v);
    public Hasher writeUInt32(uint32 v);
    public Hasher writeUInt64(uint64 v);
    public Hasher writeFloat32(float32 v);
    public Hasher writeFloat64(float64 v);
    public Hasher writeBytes(byte[] data);
    public Hasher writeBytes(byte[] data, int64 offset, int64 length);
    public Hasher writeString(String s);

    // Mixes obj.hash() into the running state. Used by the
    // compiler-synthesized Object.hash() when walking class-typed
    // fields.
    public Hasher writeObject(Object obj);

    // Finalize and produce a 64-bit value. Subsequent writes are
    // undefined — most algorithms zero or reset state after.
    public int64 finish();
}
```

### `XXHash3` — fast general-purpose

The default mixer behind `DefaultHasher` and the public choice for
non-attacker-controlled hashing (file content, internal cache keys,
ML embedding bucketing, blob deduplication, etc.). Multi-GB/s
throughput on modern CPUs; excellent distribution proven by years
of hash-table benchmark exposure (LZ4, zstd, RocksDB, ClickHouse).

```cajeta
public final class XXHash3 implements Hasher {
    public XXHash3();                                  // process-seed
    public XXHash3(int64 seed);                        // explicit seed

    // One-shot factories.
    public static int64 hash(byte[] data);
    public static int64 hash(byte[] data, int64 seed);
    public static int64 hash(byte[] data, int64 offset, int64 length, int64 seed = 0);
    public static int64 hashString(String s);
    public static int64 hashString(String s, int64 seed);

    // Hasher interface — single contract for streaming use.
    public Hasher writeInt8(int8 v);
    // ... (full Hasher surface)
    public int64 finish();
}
```

### `RapidHash` — maximum throughput, smaller code

Wang Yi's 2024 successor to wyhash. Public domain. Roughly 20-30%
faster than xxHash3 on medium-to-large inputs (~12-15 GB/s on
modern x86), smaller code footprint, passes SMHasher3 with no
statistical anomalies. Pick this when raw throughput is the
binding constraint and external interop with the xxHash3 ecosystem
isn't required: high-volume content fingerprinting, log-pipeline
keying, ML embedding bucketing where hash() is on the hot path.

Not the `DefaultHasher` backing algorithm — that role goes to
xxHash3 for ecosystem reasons (years of production exposure across
zstd / LZ4 / RocksDB / ClickHouse, reference ports in every major
language). The cost of getting `Object.hash()`'s algorithm "wrong"
is everyone's downstream tooling (heap dumps, debug logs)
producing different hash values forever after a future swap; the
conservative pick now is cheaper than pivoting later. If RapidHash
matures into the new consensus over the next few years, swapping
DefaultHasher's backing algorithm is a one-line change with no
public-API impact.

```cajeta
public final class RapidHash implements Hasher {
    public RapidHash();                                // process-seed
    public RapidHash(int64 seed);                      // explicit seed

    public static int64 hash(byte[] data);
    public static int64 hash(byte[] data, int64 seed);
    public static int64 hash(byte[] data, int64 offset, int64 length, int64 seed = 0);
    public static int64 hashString(String s);
    public static int64 hashString(String s, int64 seed);

    public Hasher writeInt8(int8 v);
    // ... (full Hasher surface)
    public int64 finish();
}
```

### `SipHash` — DoS-resistant for untrusted input

When the input is attacker-controlled (HTTP request body keys,
shared-cache lookups keyed on user-supplied strings, untrusted JSON
field names), the per-process seed alone isn't enough — an attacker
who observes hash outputs once can sometimes reverse the seed and
craft collisions. SipHash uses a 128-bit secret key with stronger
mixing rounds (SipHash-2-4, the standard variant); cryptographically
reasoned about as a PRF. Slower than xxHash3 (~600 MB/s vs ~10 GB/s)
but the right answer for those cases. Used by Rust's HashMap default
for the same reasons.

```cajeta
public final class SipHash implements Hasher {
    public SipHash(byte[16] key);                     // explicit 128-bit key
    public static SipHash withRandomKey();            // SecureRandom-sourced

    public static int64 hash(byte[] data, byte[16] key);
    public static int64 hash(byte[] data, int64 offset, int64 length, byte[16] key);
    public static int64 hashString(String s, byte[16] key);

    public Hasher writeInt8(int8 v);
    // ... (full Hasher surface)
    public int64 finish();
}
```

### `MD5` — checksum / identifier, not for security

MD5 is cryptographically broken — collisions can be constructed by
an attacker — so it must not be used for signatures, password hashes,
authentication tags, or any context where an adversary could forge
input that hashes to a target value. But MD5 remains universally
used for *non-security* purposes:

- HTTP ETags
- AWS S3 `Content-MD5` upload-integrity header
- Asset / file content fingerprinting (Git uses SHA-1 similarly —
  same trade-off: broken for crypto, fine for content addressing
  against accidental corruption)
- Cache key derivation
- Database row fingerprinting
- Test fixture / golden-output identity

Java, Python, Go, .NET all ship MD5 in their standard libraries for
exactly these reasons, and cajeta should too. The doc comment on
every `MD5` method names the security caveat in its first line.

```cajeta
public final class MD5 implements Hasher {
    public MD5();

    // Conventional MD5 use — returns the full 16-byte digest.
    public static byte[16] hash(byte[] data);
    public static byte[16] hash(byte[] data, int64 offset, int64 length);
    public static byte[16] hashString(String s);

    // Hex-encoded, 32 lowercase hex chars. The format curl and the
    // S3 SDKs (et al.) speak natively.
    public static String hashHex(byte[] data);
    public static String hashHex(byte[] data, int64 offset, int64 length);
    public static String hashStringHex(String s);

    // Streaming.
    public MD5 update(byte[] data);
    public MD5 update(byte[] data, int64 offset, int64 length);
    public byte[16] digest();
    public String   digestHex();

    // Hasher conformance — returns the first 8 bytes of the digest
    // as int64. Less useful for MD5 specifically (callers usually
    // want the full 16-byte digest); provided for generic-Hasher
    // code paths.
    public Hasher writeInt8(int8 v);
    // ...
    public int64 finish();
}
```

### `DefaultHasher` — what `Object.hash()` uses

```cajeta
public final class DefaultHasher implements Hasher {
    // Process-seeded XXHash3 underneath. The seed is mixed in at
    // construction; finish() reflects every write since.
    public DefaultHasher();
}
```

Manual `hash()` overrides thread fields into a `DefaultHasher` to
stay consistent with the synthesized default's mixing behavior:

```cajeta
public class CaseInsensitiveString {
    public String inner;

    public boolean operator==(Object obj) {
        if (!(obj instanceof CaseInsensitiveString)) return false;
        return inner.toLowerCase().equals(((CaseInsensitiveString) obj).inner.toLowerCase());
    }

    public int64 hash() {
        return new DefaultHasher()
            .writeString(inner.toLowerCase())
            .finish();
    }
}
```

### `Hash` utility namespace

```cajeta
public final class Hash {
    // Pointer-based identity hash. Returns the obj pointer's bit
    // pattern mixed with the per-process seed. Used by
    // IdentityHashMap, observer / weak-ref tables, anywhere
    // "same heap object" is the desired equality.
    public static int64 identity(Object obj);

    // Combine two 64-bit hash values into one with good
    // distribution. For manual hash() implementations that
    // can't or shouldn't go through a Hasher (rare).
    public static int64 combine(int64 a, int64 b);

    // The process-wide random seed initialized from OS entropy at
    // process start. The compiler-synthesized hash() implicitly
    // uses this; exposed here for direct use cases that need
    // alignment with the synthesized hashing.
    public static int64 processSeed();
}
```

### Primitive specializations

Compiler intrinsics, not real algorithm calls:

| Type        | `hash()` returns                                                |
|-------------|-----------------------------------------------------------------|
| `int8` / `int16` / `int32` / `int64` | value mixed with the process seed (one multiply + xor) |
| `uint8` / `uint16` / `uint32` / `uint64` | same as the signed variant — bit pattern is what matters |
| `float32` / `float64` | bitcast to integer, canonicalize ±0 → 0, then mix         |
| `boolean`   | 0 or 1 mixed with seed                                          |
| `pointer`   | `Hash.identity(ptr)` — pointer mixed with seed                  |
| `String`    | xxHash3 over UTF-8 bytes, process-seeded                        |
| `byte[N]`   | xxHash3 over the N bytes, process-seeded                        |
| class types | `obj.hash()` — recurses into the synthesized / overridden hash  |

### When to pick which

| Use case                                              | Pick                |
|-------------------------------------------------------|---------------------|
| Default Object.hash() (compiler does this)            | `DefaultHasher`     |
| Hash a buffer / file / blob for fingerprinting        | `XXHash3.hash`      |
| Cache key derivation, internal table keys             | `XXHash3.hash`      |
| High-volume hashing, no external interop needed       | `RapidHash.hash`    |
| Untrusted input (HTTP body, user strings, etc.)       | `SipHash.hash`      |
| HTTP ETag / S3 Content-MD5 / asset fingerprinting     | `MD5.hashHex`       |
| Identity-based (graph nodes, weak refs)               | `Hash.identity`     |
| Real cryptographic security (signatures, passwords)   | **`cajeta.crypto`** (separate library) |

### Future peer: `cajeta.crypto`

A separate first-party library covering the cryptographic
primitives that don't fit in `cajeta.hash`:

- **Cryptographic hashes**: SHA-1 (deprecated), SHA-2 family
  (SHA-256, SHA-384, SHA-512, SHA-224), SHA-3 family + SHAKE,
  BLAKE2b / BLAKE2s, BLAKE3.
- **HMAC** over the above.
- **AEAD**: AES-128 / AES-256 (GCM, CCM, CTR), ChaCha20-Poly1305.
- **Asymmetric**: Ed25519 (signatures), X25519 (key agreement),
  RSA with strong defaults.
- **KDFs**: PBKDF2, scrypt, Argon2, HKDF.
- **Constant-time primitives** — `cajeta.crypto.constantTimeCompare`,
  zero-on-drop byte arrays for secret material.

Lives separately because cryptographic API churn (algorithm
deprecation, side-channel mitigations, post-quantum migration)
moves faster than stdlib stability promises should accept, and
many cajeta programs don't need any of it.

---

## cajeta.io

The buffer infrastructure your server harness will need:

```cajeta
public class Buffer {
    public Buffer(int64 capacity);
    public int64 capacity();
    public byte[] data();
    public Buffer next();
    public void linkNext(Buffer next);
}

public class BufferChain {
    public void append(Buffer b);
    public Buffer head();
    public int64 totalSize();
    public Iterator<Buffer> iterator();
}
```

`Buffer` wraps a single `byte[MAX_SIZE]`. `BufferChain` is the linked list of
buffers you described. A struct overlay (via the existing
`MyStruct(byte[] bytes)` zero-copy view) reads/writes fixed-layout records out
of any `Buffer`.

`InputStream` / `OutputStream` / `Reader` / `Writer` follow Java's shape but
with byte buffers as the underlying mechanism. These can land alongside the
harness once `cajeta.net` arrives — they're not blocking the harness if it
uses `Buffer` / `BufferChain` directly.

---

## cajeta.thread

Documented separately in cajeta-docs/ThreadModel.md. Stub here:

```cajeta
public final class Fiber {
    // Cooperative sleep. Parks the calling fiber on the timer wheel
    // for at least `millis`; the carrier runs other fibers in the
    // meantime. Wakeup is best-effort — actual delay >= requested.
    public static void sleep(int64 millis);

    // Yield the carrier to other ready fibers without blocking.
    public static void yield();

    public static Fiber current();
    public int64 id();
}

public final class Thread {
    // Blocking sleep on the OS thread. Used outside fiber context
    // (the harness's main / control thread). Inside a fiber, prefer
    // Fiber.sleep so other fibers can run.
    public static void sleep(int64 millis);
}
```

---

## cajeta.error

Already shipping (currently inline in `Compiler.cpp` as `STDLIB_SOURCE`).
Externalization moves the existing hierarchy to disk verbatim:

```
runtime/src/cajeta/error/Throwable.cajeta
runtime/src/cajeta/error/Exception.cajeta
runtime/src/cajeta/error/RecoverableException.cajeta
runtime/src/cajeta/error/UnrecoverableException.cajeta
```

While we're moving these, the parse-stdlib-once-per-Compiler change lands
alongside (see ThreadModel.md / multi-source compile work — the per-module
re-parse is what forced `OverrideFromSrc` in the JIT helper).

---

## Implementation sequence

A reasonable order, given dependencies:

1. **Externalize cajeta.error to disk.** Smallest move. Includes the
   parse-stdlib-once refactor and drops `OverrideFromSrc` in JitTestHelper.
2. **cajeta.lang: Object decision, Encoding enum, String skeleton.** String
   methods that don't depend on collections (substring, indexOf,
   getBytes/fromBytes, count, equals). Boxed primitives.
3. **cajeta.collection interfaces + array hookup.** `Iterable`, `Iterator`,
   `Collection`, plus the compiler-side wiring so arrays implement
   `Collection<T>`.
4. **cajeta.time Clock methods.** `Clock.nanoTime`, `Clock.millisTime` —
   trivial intrinsic wrappers. Enough for the harness.
5. **cajeta.collection: ArrayList, LinkedList, ArrayDeque, ArrayStack,
   HashMap, HashSet, BitSet.** Daily-use containers.
6. **cajeta.thread: Fiber.sleep, Fiber.yield, Thread.sleep, nanoTime.**
   The reactor + timer wheel land here.
7. **cajeta.io: Buffer, BufferChain.** Server harness consumes these.
8. **cajeta.time value types.** Instant first, then Duration, then the
   Local* types, then ZoneId / ZonedDateTime / DateTimeFormatter.
9. **cajeta.collection: TreeMap, TreeSet, DenseMap/Set, Heap, immutable
   variants, trees.** Less common; ship as needed.

Steps 1-6 unblock the server harness. Steps 7-9 fill out the library.

---

## Open questions for review

1. ~~`Object` as root?~~ **Decided: yes.** Universal root with three methods —
   `operator==(Object)`, `hash()`, `toString(Encoding)` — all with compiler-
   synthesized structural defaults. Pair enforcement on operator==/hash; cycle
   detection at compile time with `@transient` opt-out per field.
2. **String internal encoding** — UTF-8 vs UTF-16. UTF-8 is more efficient
   for ASCII-dominant content, more standard on the wire. UTF-16 makes
   code-point indexing O(1) for the BMP. Recommended: UTF-8, with
   `codePointAt(index)` being O(index) — accept the trade-off, document it.
3. **Sparse vs dense as separate types or as builder options?** Recommended:
   builder options (smaller hierarchy). Same answer for "ordered" vs
   "hashed" — already separate types because the algorithms differ; the
   sparse/dense distinction inside hashed doesn't warrant a separate
   public type.
4. **Immutable variants** — separate subclasses vs annotation. Recommended:
   separate subclasses for now; the annotation form requires escape analysis
   we don't have.
5. **Time zone database** — embed vs read from disk. Recommended:
   read-from-disk with UTC + fixed-offset baked in. Document the dependency.
6. **`Array.count()` implementation** — compiler intrinsic or synthesized
   method on the array's class. Recommended: compiler intrinsic on the
   array type, dispatched the same way other intrinsics are. Adding a real
   method per element type would balloon the symbol table.

Once these settle, the package files start landing.
