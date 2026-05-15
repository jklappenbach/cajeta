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
  growable vs frozen) instead of one "Map" / one "Set" that hides the trade-off.
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
                         Array (the heap-allocated, variable-size,
                         element-typed array — replaces T[] for non-inline use);
                         ArrayList, LinkedList, HashSet, TreeSet, DenseSet,
                         SparseSet, HashMap, TreeMap, DenseMap, SparseMap,
                         ArrayDeque, LinkedDeque, ArrayStack, BitSet;
                         immutable.{List,Set,Map,Deque,Array} read-only variants;
                         tree.{BinaryTree, RedBlackTree, BTree, BPlusTree}
cajeta.io              — InputStream, OutputStream, Reader, Writer, byte buffers;
                         the linked-list-of-buffers shape used by network code
cajeta.net             — Socket, ServerSocket (later — needs the reactor)
cajeta.concurrent      — Thread, Fiber, Sleep, Mutex (extends what
                         ThreadModel.md already documents)
cajeta.math            — Integer / Long / Double parse helpers, BigInteger,
                         basic math utility methods (already partly intrinsic)
```

---

## cajeta.lang

### `String`

Immutable, encoding-aware character sequence. Internal storage: UTF-8 byte
array plus a cached code-point count. Decision rationale below.

```cajeta
public final class String implements Collection<int32> {
    // Construction
    public String();
    public String(byte[] bytes, Encoding encoding);
    public static String fromCodePoints(int32[] codePoints);
    public static String repeat(String s, int64 n);

    // Inspection
    public int64 count();                          // code-point count
    public int64 byteCount();                      // raw byte count
    public boolean isEmpty();
    public int32 codePointAt(int64 index);         // O(index) on UTF-8
    public boolean equals(String other);
    public int32 compare(String other);
    public int64 hash();

    // Search
    public int64 indexOf(String needle);
    public int64 indexOf(String needle, int64 fromIndex);
    public int64 lastIndexOf(String needle);
    public boolean contains(String needle);
    public boolean startsWith(String prefix);
    public boolean endsWith(String suffix);

    // Transformation (return new String)
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

    // Iteration — yields code points in order
    public Iterator<int32> iterator();
}
```

`String` implements `Collection<int32>` so `for (cp in someString) { ... }`
works and so `count()` is consistent across the rest of the collection
hierarchy. Iteration is over code points, not bytes — bytes are accessible via
`getBytes(Encoding.UTF_8)` when needed.

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

### `Array<T>` implements `Collection<T>`

This is the load-bearing piece of "arrays included in Collection polymorphism."
Concretely:

- `int32[]`, `byte[]`, `Foo[]`, etc. all carry a length in their header
  (already true today — `__cajeta_new_array` does this). `count()` reads that
  length.
- The compiler synthesizes an `iterator()` for any array type that yields
  elements in index order.
- Code that takes `Collection<int32>` accepts an `int32[]` directly. No
  wrapping.

Open question: does this require runtime magic (compiler treats `T[]` as
having `Collection<T>` methods) or does it work through normal
implements-interface resolution? Recommended: compiler-level — the array
struct has a vtable slot that points at a generated `count`/`iterator` pair
for the element type, so `(Collection<T>) myArr` dispatches normally.

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
- **`ArrayList<T>`** — growable array, amortized O(1) append.
- **`LinkedList<T>`** — doubly-linked, O(1) insert/remove at known position,
  O(1) at-ends. Also implements `Deque<T>`.

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
  Implements `Set<int32>`.

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
- **`ArrayDeque<T>`** — ring-buffer-backed Deque. Default.
- **`LinkedDeque<T>`** — alias for `LinkedList<T>` (which already qualifies).
- **`ArrayStack<T>`** — Stack on top of ArrayList. The "use a List as a Stack
  manually" pattern wrapped.

### Frozen / immutable variants

In a `cajeta.collection.frozen` sub-package:

```cajeta
public final class FrozenList<T> extends ArrayList<T> { ... }
public final class FrozenSet<T> extends HashSet<T> { ... }
public final class FrozenMap<K, V> extends HashMap<K, V> { ... }
```

Mutators (`add`, `put`, `remove`, …) throw `UnsupportedOperationException`. The
frozen types have stronger compiler optimizations available because element
identity is stable — no rehash, no resize, no concurrent-modification checks.

Alternative design: a `Frozen<T>` annotation on a collection variable that
the compiler treats as immutable. Cleaner syntactically; harder to enforce
once values escape across method boundaries. Recommended: separate concrete
types, simpler.

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

## cajeta.concurrent

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
6. **cajeta.concurrent: Fiber.sleep, Fiber.yield, Thread.sleep, nanoTime.**
   The reactor + timer wheel land here.
7. **cajeta.io: Buffer, BufferChain.** Server harness consumes these.
8. **cajeta.time value types.** Instant first, then Duration, then the
   Local* types, then ZoneId / ZonedDateTime / DateTimeFormatter.
9. **cajeta.collection: TreeMap, TreeSet, DenseMap/Set, frozen variants,
   trees.** Less common; ship as needed.

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
4. **Frozen variants** — separate subclasses vs annotation. Recommended:
   separate subclasses for now; the annotation form requires escape analysis
   we don't have.
5. **Time zone database** — embed vs read from disk. Recommended:
   read-from-disk with UTC + fixed-offset baked in. Document the dependency.
6. **`Array.count()` implementation** — compiler intrinsic or synthesized
   method on the array's class. Recommended: compiler intrinsic on the
   array type, dispatched the same way other intrinsics are. Adding a real
   method per element type would balloon the symbol table.

Once these settle, the package files start landing.
