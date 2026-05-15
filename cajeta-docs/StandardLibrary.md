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
                         StructView marker for zero-alloc views over
                         external buffers;
                         Array (the heap-allocated, variable-size,
                         element-typed array — replaces T[] for non-inline use);
                         ArrayList, LinkedList, HashSet, TreeSet, DenseSet,
                         SparseSet, HashMap, TreeMap, DenseMap, SparseMap,
                         ArrayDeque, LinkedDeque, ArrayStack, BitSet, Heap;
                         Immutable[List,Set,Map,Deque,Array,Heap] read-only variants;
                         tree.{BinaryTree, RedBlackTree, BTree, BPlusTree}
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
Object. Three methods, all with compiler-synthesized structural defaults so
the common case requires no boilerplate:

```cajeta
public class Object {
    public boolean operator==(Object obj);
    public int64 hash();
    public String toString(Encoding e = UTF_8);
}
```

**Default implementations are structural, not identity-based.** Compiler
synthesizes:

- `operator==(Object obj)`: instanceof-check against the declaring class, cast,
  field-by-field comparison. The Java pattern "always implement equals with
  null-check, instanceof, cast, compare" — written for you by the compiler.
- `hash()`: combines the same field set the structural `operator==` consults,
  using a fast non-cryptographic mixer (FxHash family). Stable for the
  lifetime of the values being hashed; not stable across process restarts (no
  randomized seed yet, but plan to add per-process seed for hash-flooding
  defense).
- `toString(Encoding)`: `TypeName(field1=value1, field2=value2, ...)`. The
  Rust `#[derive(Debug)]` shape, sufficient for `println(x)` debug output.
  Encoding parameter defaults to UTF-8; other encodings supported via the
  default-argument mechanism cajeta already has.

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
keys — goes through `Cajeta.identityHash(obj)` as a runtime intrinsic, and
`IdentityHashMap<K, V>` ships as a separate type that uses it internally. No
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

### Immutable variants

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

In `cajeta.collection.tree`. These don't appear in everyday code but support
"data larger than memory" patterns:

- **`BinaryTree<T>`** — bare binary tree, no balancing. The building-block
  type if you want to write your own algorithms; rarely the right answer
  on its own.
- **`RedBlackTree<T>`** — backs `TreeSet` and `TreeMap`.
- **`BTree<K, V>`** — disk-friendly multi-way tree. Use for ordered
  collections that don't fit in memory.
- **`BPlusTree<K, V>`** — leaf-linked variant, optimized for range scans.

v1: ship `RedBlackTree` (we need it for `TreeSet` / `TreeMap` anyway).
`BinaryTree` is trivial — also ship. `BTree` / `BPlusTree` defer until
someone actually uses them.

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
