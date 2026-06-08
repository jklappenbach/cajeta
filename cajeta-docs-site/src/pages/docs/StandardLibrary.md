---
title: 'Standard Library'
layout: '~/layouts/MarkdownLayout.astro'
category: 'Stdlib'
description: 'A design for cajeta''s standard library — what packages exist, what types live in each, and the shape of their public surface. This document captures intent; implementation lands incrementally as separ...'
---

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
  already specifies — covered in detail in **CajetaReflect.md**. The
  `cajeta.reflect` package surfaces the RTTI tables the compiler already
  emits (Class, Field, Method, Constructor, Annotation, generic retention,
  reflective invoke). Listed here in non-goals because the stdlib package
  has its own design doc; not because reflection itself is out of scope.
- A regex engine in this pass — defer to a later library.
- Async primitives beyond `Thread.sleep` / `Fiber.sleep` — full reactor lives
  in docs/ThreadModel.md.

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
cajeta.io              — root I/O umbrella: Buffer, BufferChain (linked-
                         list-of-buffers used by network code);
                         InputStream, OutputStream, Reader, Writer
                         abstractions; encoding-aware text codecs.
                         Concrete I/O kinds live in subpackages below.
cajeta.io.file         — Path, File, FileInfo, OpenMode, Watcher,
                         FileEvent; readText / writeBytes / glob / walk /
                         copy / move / delete; atomic writes by default;
                         the @capability("filesystem")-gated surface.
cajeta.io.net          — Address, SocketAddress, InetAddress (v4 / v6),
                         Socket, ServerSocket, Selector — the reactor
                         surface. Needs the fiber reactor; lands with
                         the harness work.
cajeta.io.net.tcp      — TcpSocket, TcpServerSocket, TcpListener,
                         TcpStream.
cajeta.io.net.udp      — UdpSocket, DatagramPacket.
cajeta.io.net.tls      — TlsSocket wrapping any net socket; TlsContext,
                         Certificate, PrivateKey.
cajeta.io.net.http     — HTTP/1.1 + HTTP/2 + Server-Sent Events; full
                         design in CajetaHttp.md.
cajeta.io.net.websocket — WebSocket protocol with the http upgrade
                         integration; also in CajetaHttp.md.
cajeta.thread          — Thread, Fiber, Sleep, Mutex (extends what
                         ThreadModel.md already documents)
cajeta.process         — Process, ProcessBuilder, Stdio, ExitStatus,
                         Signal — subprocess management with fiber-aware
                         stream pumps. Sibling of cajeta.io and
                         cajeta.thread (not nested under either).
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
cajeta.reflect         — runtime type introspection: Class<T>, Field,
                         Method, Constructor, Parameter, Annotation,
                         TypeParameter / TypeArgument (with generic
                         retention, not erasure), Modifiers; reflective
                         get / set / invoke / newInstance; Class.forName
                         lookup; @Reflectable / @Retained / UnsafeReflect
                         access-control surface. See CajetaReflect.md
                         for the full design.
```

---

## cajeta.lang

### `String`

Immutable, encoding-aware character sequence. Internal storage: UTF-8 byte
array plus a cached code-point count. A `String` carries a tagged mode
internally — **owned** (heap-allocated; `heap String(...)`) or **view**
(borrowed over bytes that live elsewhere; `String.viewOf(...)`).

```cajeta
public final class String implements Collection<int32> {
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

#### Where owned and view Strings meet

Both modes are the same type, so user code mostly doesn't notice.
The differences show up at the boundaries:

- **Allocation.** `heap String(...)` allocates and copies. `String.
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
Object via the auto-extend pass in `CajetaLlvmVisitor::visitClassDeclaration`.
Four methods, **identity-based defaults** (Java's `Object.hashCode()` shape,
not Rust's `derive(Hash)`):

```cajeta
public class Object {
    public boolean operator==(Object obj);
    public int64 hash();
    public String toString(Encoding e = UTF_8);
    public Object clone();
}
```

**Defaults are identity-based, not structural.** Two distinct instances with
the same field values produce different hashes and compare unequal by default.
This avoids the cliff of structural-by-default — where the compiler would have
to synthesize correct walks across primitive, class-reference,
array, generic, and self-referential fields, with each new field shape
introducing a new edge case the synthesizer has to get right *silently* (a
broken structural hash doesn't crash, it just returns wrong results, and
HashMap quietly misbehaves).

The identity-default is paired with explicit opt-in: a class that wants
value-keyed semantics either **overrides `hash()` manually** or applies the
**`@AutoHash` annotation** (see below).

- `operator==(Object obj)`: pointer identity by default. Override for value
  equality.
- `hash()`: pointer-identity hash mixed with the per-process random seed
  (initialized from OS entropy at process start). Hash values are stable for
  the object's lifetime within a process but not across restarts. The seed
  kills hash-flooding attacks: an attacker can't predict bucket placement to
  force pathological O(n²) HashMap behavior.
- `toString(Encoding)`: `TypeName@<address>` shape by default (debug-only).
  Override for user-facing representation.
- `clone()`: shallow field-by-field copy. For value-typed fields the copy
  is a memcpy of the bits; for class-typed fields the copy is a shallow
  reference copy (both originals point at the same heap instance — Java's
  `Object.clone()` shallow path).

**Override pair enforcement.** If a class declares `operator==` manually,
it must also declare `hash()` (and vice versa). The contract — equal values
hash equally — is structurally protected by requiring both halves to be
authored together. `toString` has no pair requirement; override it
independently.

**`Comparable<T>` stays as an opt-in interface** because natural ordering is
domain-specific — there's no sensible compiler default for "compare a class
with an int field and a String field." `TreeMap<K, V>` and `TreeSet<T>` carry
`K extends Comparable<K>` as a bound; HashMap and HashSet do not.

#### `@AutoHash` — opt-in structural hashing

Apply `@AutoHash` to a class to have the compiler synthesize a structural
`hash()` (and, paired, a structural `operator==`) by walking the class's
fields. This is the cajeta equivalent of Rust's `#[derive(Hash, Eq)]`,
Swift's automatic `Hashable` conformance, Kotlin's `data class` — opt-in,
not default.

```cajeta
@AutoHash
public class Point {
    public int32 x;
    public int32 y;
}

// Compiler synthesizes:
//   public int64 hash() {
//       int64 acc = Hash.processSeed();
//       acc = Hash.combine(acc, Hash.int32(this.x));
//       acc = Hash.combine(acc, Hash.int32(this.y));
//       return acc;
//   }
//   public boolean operator==(Object other) { ... fieldwise compare ... }

HashMap<Point, String> map = heap HashMap<>();   // Just works
```

**Error attribution is load-bearing.** Synthesizer-emitted diagnostics must
name the **annotated class**, the **offending field**, and the **specific
reason** — a user looking at the error should know exactly what to change
without reading generated code:

```
error: @AutoHash on `app.User`: field `groups` (type `LinkedList<Group>`)
       cannot be auto-hashed — collection types need either a manual
       hash() override on `User` or @AutoHash on `Group` plus a
       hashable element walk that v1 doesn't synthesize.
       at app/User.cajeta:14:5

error: @AutoHash on `tree.Node`: field `parent` (type `tree.Node`)
       creates a cycle through self. Mark the field `@transient` to
       exclude it from the hash walk, or implement hash() manually.
       at tree/Node.cajeta:8:5
```

The annotation processor:

- Carries the annotation's source location (file:line:col) through the
  synthesizer so every emitted diagnostic points at the user's `@AutoHash`,
  never at compiler internals.
- Names the offending field by source identifier, not by LLVM index.
- Includes a "what to do" line — the user shouldn't need to read this doc to
  fix the error.

**Cyclic-type detection.** The synthesizer walks the class's field type graph.
If any field type can transitively reach back to the class itself, refuse to
emit and produce the diagnostic above. Two user fixes:

- **`@transient` field annotation** — the synthesizer skips the annotated
  field when walking fields for hash/equals. Java borrowed `transient` from
  for-serialization-skip; cajeta repurposes it for "compiler shouldn't
  traverse this when synthesizing." Cheap fix for the common case (one
  back-reference closes the cycle).
- **Manual implementation** — for complex shapes (mutual recursion across N
  classes, diamonds, memoizing traversal). User implements both `hash()` and
  `operator==`.

The cycle analysis also runs through generic instantiations
(`LinkedList<Node>` where `Node` references `LinkedList<Node>` is a cycle).

**Field-kind coverage** for the v1 `@AutoHash` synthesizer:

| Field kind                       | Status                                    |
|----------------------------------|-------------------------------------------|
| Primitives (int*, float*, bool)  | Hashed via `__cajeta_hash_*` helpers      |
| Class types (`Foo`)              | Virtual dispatch to `field.hash()`        |
| Array / collection types         | Element walk via iterator                 |
| `String`                         | Byte hash via XXH3-64                     |
| `@transient` field               | Skipped                                   |
| Self-referential (cycle)         | Compile error unless cycle broken         |

**Identity hash as a separate intrinsic.** The rare case that genuinely wants
pointer-based hashing — observer maps, graph node identity, weak-reference
keys — goes through `cajeta.hash.Hash.identity(obj)` as a runtime intrinsic,
and `IdentityHashMap<K, V>` ships as a separate type that uses it internally.
No `Hashable` bound on K because IdentityHashMap doesn't ask K to hash itself.

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
    public Iterator<T> iter();
    public Iterator<T> iterOwned();   // consuming iteration
}

public interface Collection<T> extends Iterable<T> {
    public int64 count();
}
```

`Iterable` is separated so types that only need "give me an iterator" don't
inherit `count()`. Streams, generators, file lines — those are `Iterable`
but not `Collection`. The `Iterator<T>` design follows §"Iterator design"
below — single-call `next()` returning `Optional<T>`, baked-in combinators,
mutation-during-iteration is a compile error.

### Iterator design

Iterators in cajeta deliberately don't repeat the design mistakes other
languages have made. Eight principles, in order from "must" to "nice":

#### 1. Single-call `next()` returning `Optional<T>`

The Rust shape, not the Java shape. One call per element, termination
encoded in the return value — no separate `hasNext()` + `next()` pair (two
virtual calls per element, the perennial Java tax), no
exception-on-termination (Python's StopIteration overhead). Implementations
that don't have a cheap probe-before-take get a single `next()` to
implement.

```cajeta
public interface Iterator<T> {
    public Optional<T> next();
    // ... default-method combinators (see §3)
}

// Hand-rolled consumption:
Iterator<int32> it = xs.iter();
loop {
    match it.next() {
        Some(x) => print(x),
        None    => break,
    }
}
```

Languages compared: **Java/Kotlin/C#** `hasNext()+next()` is the
expensive two-call protocol. **Python** `__next__()` raising
`StopIteration` is a single call but pays exception teardown per
iteration. **Rust/Swift** match the cajeta shape.

Concrete implementation — an array iterator declared as a `struct`
(see `Structs.md`), implementing the `Iterator<T>` interface via
the tagged-fat-pointer dispatch:

```cajeta
public struct ArrayIter<T> implements Iterator<T> {
    private T[]   data;        // borrowed array reference
    private int64 idx;
    private int64 stop;

    public Optional<T> next() {
        if (this.idx >= this.stop) return Optional<T>.None();
        T v = this.data[this.idx];
        this.idx += 1;
        return Optional<T>.Some(v);
    }
}
```

Direct calls on `ArrayIter<T>` are monomorphized (zero vtable cost,
inlinable). Calls through `Iterator<T>` go via the interface value's
vtable — one indirection. Both work; pick based on whether the call
site has a concrete type or an interface-typed receiver.

#### 2. Combinators are default methods on `Iterator<T>`, present from day one

`map`, `filter`, `fold`, `find`, `count`, `take`, `skip`, `chain`,
`enumerate`, `reduce`, `any`, `all`. Each returns a type that wraps
the inner iterator and implements `Iterator<T>` itself — composes lazily
with no per-element allocation.

```cajeta
public interface Iterator<T> {
    public Optional<T> next();

    // Default-method combinators. Each returns a wrapper that
    // adapts `this` and re-implements `next()`. Lazy: nothing happens
    // until the consumer pulls.

    public MapIter<T, U> map<U>((T) -> U fn) {
        return MapIter<T, U>(this, fn);
    }

    public FilterIter<T> filter((T) -> boolean pred) {
        return FilterIter<T>(this, pred);
    }

    public Optional<T> find((T) -> boolean pred) {
        // Eager — drains until a match or end.
        for (x in this) {
            if (pred(x)) { return Some(x); }
        }
        return None();
    }

    public int64 count() {
        int64 n = 0;
        for (_ in this) { n = n + 1; }
        return n;
    }

    // ... etc.
}

// Usage — chain composes without per-element heap traffic:
int32 sumOfSquares = xs.iter()
    .filter((x) -> x > 0)
    .map((x) -> x * x)
    .fold(0, (acc, x) -> acc + x);
```

Languages compared: **Java Stream API**, **C# LINQ** — combinators
added years after the iterator protocol; never as ergonomic as if they
were native. **Rust/Swift** ship combinators on the trait from v1.
Cajeta does the same.

#### 3. Borrow-check integration — mutation during iteration is a compile error

`m.iter()` registers a borrow on `m` via cajeta's existing borrow
detection (the same machinery that catches `T[] alias = paramArr`). Any
mutating call on `m` while the iterator is live is rejected at compile
time. No `ConcurrentModificationException` runtime band-aid, no
undefined behavior.

```cajeta
HashMap<int32, String> m = ...;
Iterator<Entry<int32, String>> it = m.iter();
for (entry in it) {
    m.put(entry.key + 1, "added");  // ← compile error:
                                     //    cannot mutate `m` while
                                     //    `it` (line N) borrows it
}
```

Languages compared: **Java's ConcurrentModificationException** detects
this *at runtime* on the next iteration. **Python** is undefined
behavior. **Rust** rejects at compile time (which is what cajeta does).
**C++** is undefined behavior with iterator-invalidation rules nobody
remembers.

#### 4. Multi-yield via destructuring an `Entry` value, no special syntax

`HashMap` iteration yields a single value per call — an `Entry<K, V>`.
The `for (k, v in m)` Go-style form is sugar over destructuring that
single entry. Same machinery handles N-element tuples
(`Iterator<Triple<A, B, C>>`).

```cajeta
public struct Entry<K, V> {
    public K key;
    public V value;
}

// HashMap exposes both forms:
for (entry in m.iter()) {
    print(entry.key);
    print(entry.value);
}

// Destructuring sugar:
for (k, v in m.iter()) {
    print(k);
    print(v);
}
```

Languages compared: **Go** special-cases `for k, v := range m` as a
language form — not extensible. **Python** unpacks tuples in `for k, v
in m.items()` but the protocol uses `.items()` as a separate iter
method. **Rust** uses `&(k, v)` destructuring on the tuple iterator
type. Cajeta's `Entry` + destructuring sugar gives the same
ergonomics without special-casing maps.

#### 5. No `IntoIterator` vs `Iterator` split

Rust's three-method `iter()` / `iter_mut()` / `into_iter()` is honest
but a teaching obstacle. Cajeta has two:

- `iter()` — borrowing iteration; container outlives the iterator.
- `iterOwned()` — consuming iteration; takes ownership of the container,
  yields owned elements, container is gone after the loop.

Mutating iteration is a separate concern handled by the borrow checker
plus a `set(idx, T)`-style method on the underlying container.

```cajeta
// Borrowing — the common case
for (x in xs.iter()) {
    print(x);
}
print(xs.count());  // still usable

// Consuming — when you want to drain into something else
HashMap<int32, String> source = ...;
HashMap<int32, String> dest = heap HashMap<int32, String>(64);
for (entry in source.iterOwned()) {
    dest.put(entry.key, entry.value);
}
// `source` is gone here — moved into the iter, drained by the loop
```

Languages compared: **Rust** three traits (`iter`, `iter_mut`,
`into_iter`) — explicit but heavy. **Java/Kotlin** only borrow-iterate;
no consuming form. Cajeta picks the middle: two methods, the common
distinction, no triple-trait taxonomy.

#### 6. No "end iterator"

The C++ `begin()` / `end()` pair is the source of unending off-by-one
bugs. Single-call `next()` returning `Optional<T>` removes the concept
entirely.

```cajeta
// C++ style (don't do this):
//   for (auto it = v.begin(); it != v.end(); ++it) { ... }
//
// Cajeta:
for (x in v.iter()) { ... }
//
// Or hand-rolled:
Iterator<T> it = v.iter();
loop {
    match it.next() {
        Some(x) => /* use x */,
        None    => break,
    }
}
```

Languages compared: **C++** end-iterator footgun. **Everyone else** —
including cajeta — terminates on a falsy signal from `next()`.

#### 7. `for` desugars but exposes the iterator binding

Foreach syntax is sugar over `loop { match it.next() { ... } }`, but
the iterator binding stays in scope. Callers can `.peek()`, `.skip(1)`,
or check `.count()` from inside the body. Or use the explicit `loop` /
`match` form if they need finer control.

```cajeta
// Sugar form, iterator binding exposed:
for (x in xs.iter() as it) {
    if (x < 0) {
        it.skip(3);   // skip the next 3 negatives
        continue;
    }
    process(x);
}

// Equivalent without sugar:
Iterator<T> it = xs.iter();
loop {
    match it.next() {
        Some(x) => {
            if (x < 0) { it.skip(3); continue; }
            process(x);
        },
        None => break,
    }
}
```

Languages compared: **Java** foreach hides the iterator — accessing it
requires writing the explicit loop. **Python** same — `iter()` + `next()`
manually. **Rust** has the same hiding problem (`for x in iter` doesn't
bind the iter). Cajeta's `as it` clause is opt-in; most loops won't
need it, but when you do, it's there.

#### 8. Index iteration via `enumerate`, not a separate counter

`for ((i, x) in arr.iter().enumerate())` instead of
`for (i in 0..arr.length) { x = arr[i]; ... }`. Removes the "do I want
indices or elements?" question — combinators handle both. `enumerate()`
returns an iterator of `Entry<int64, T>` (using the same Entry type
as map iteration, but with index for key).

```cajeta
// Sum of (index * value):
int64 weighted = xs.iter()
    .enumerate()
    .fold(0, (acc, e) -> acc + (e.key * e.value));

// With destructuring:
for (i, x in xs.iter().enumerate()) {
    print(i);
    print(x);
}
```

Languages compared: **Java** writes an external counter or
`IntStream.range(0, list.size())`. **Python** has `enumerate()` as a
builtin — same ergonomics, different surface. **Rust** has
`.enumerate()` on iterators. Cajeta matches the Rust/Python ergonomics
on top of the structural iterator.

### Prerequisites

One stdlib piece has to land before the iterator interface ships:

1. **`Optional<T>`** — `Some(T)` / `None()` value-typed sum, declared as
   a `struct` (see `Structs.md`), no heap allocation. `Iterator.next()`
   returns it. Independently useful for many things beyond iterators.

Map iteration yields `Entry<K, V>` values, also declared as a `struct`
— see the `Map<K, V>` section below. Both `Optional<T>` and `Entry<K, V>`
are stack-allocated value bundles; method calls on them are direct
(monomorphized), and they can participate in interface dispatch via the
tagged-fat-pointer mechanism documented in `Structs.md`.

### `Array<T>` implements `Collection<T>`

The load-bearing piece of "arrays included in Collection polymorphism."

- `int32[]`, `byte[]`, `Foo[]`, etc. all carry a length in their header
  (already true today — `__cajeta_new_array` does this). `count()` reads that
  length.
- The compiler synthesizes an `iterator()` for any array type that yields
  elements in index order.
- Code that takes `Collection<int32>` accepts an `int32[]` directly. No
  wrapping.

```cajeta
public final class Array<T> implements Collection<T> {
    public Array(int64 length);

    public T at(int64 index);
    public void set(int64 index, T value);
    public int64 count();
}
```

Open question: does Array's interface participation require runtime magic
(compiler treats `T[]` as having `Collection<T>` methods) or does it work
through normal implements-interface resolution? Recommended: compiler-level
— the array carries a vtable slot pointing at a generated
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

public struct Entry<K, V> {
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
  IdentityHashMap<GraphNode, VisitState> seen = heap IdentityHashMap();
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
- **`ArrayDeque<T>`** — ring-buffer-backed Deque. Default.
- **`LinkedDeque<T>`** — alias for `LinkedList<T>` (which already qualifies).
- **`ArrayStack<T>`** — Stack on top of ArrayList.

### `Heap<T>` — priority queue

The binary-heap-backed priority queue, missing from earlier drafts.
Backing storage is a single contiguous `Array<T>` with the standard
heap-order invariant (parent at index `i`, children at `2i+1` and
`2i+2`); operations are O(log n) push / pop, O(1) peek, O(n) heapify.

```cajeta
public final class Heap<T> implements Collection<T> {
    // Default ordering uses `Comparable<T>`; supply a comparator
    // to override.
    public Heap();
    public Heap(Comparator<T> cmp);
    public Heap(Array<T> items);              // O(n) heapify

    public static Heap<T> minHeap();
    public static Heap<T> maxHeap();

    public void push(T value);
    public T    pop();
    public T    peek();
    public int64 count();
    public boolean isEmpty();

    public Iterator<T> drainSorted();         // destructive
    public Iterator<T> iterator();            // heap order, NOT sorted
}
```

Usage — building up a priority queue:

```cajeta
Heap<int64> nextAt = Heap<int64>.minHeap();
nextAt.push(timestamp + 1000);
nextAt.push(timestamp + 250);
int64 due = nextAt.peek();        // 250-from-now wins
nextAt.pop();
```

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
events.sort(heap StoogeSort<Event>(), byTimestamp);
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

Alternative design: an `@Immutable` annotation on a collection variable
that the compiler treats as read-only. Cleaner syntactically; harder to
enforce once values escape across method boundaries. Recommended:
separate concrete types, simpler.

### Trees

In `cajeta.collection.tree`.

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
        return heap DefaultHasher()
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

Umbrella for everything that crosses the program / outside-world boundary.
Direct members are the shared abstractions; concrete I/O kinds (file,
network, subprocess) live in nested subpackages so a program that only
needs files doesn't drag in a TLS stack.

### Buffer + BufferChain — the byte substrate

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

`Buffer` wraps a single `byte[MAX_SIZE]`. `BufferChain` is the linked list
of buffers used by the harness.

### Streams: InputStream / OutputStream / Reader / Writer

Java-shaped abstractions, byte buffers underneath. `InputStream` /
`OutputStream` are byte-level; `Reader` / `Writer` wrap them with an
`Encoding` to give code-point-level access. The same interfaces back files,
sockets, subprocess stdio, and in-memory byte streams — every concrete I/O
type implements them so generic code (compress/decompress, parse/serialize)
works over any source.

These can land alongside the harness once `cajeta.io.net` arrives —
they're not blocking the harness if it uses `Buffer` / `BufferChain`
directly.

---

## cajeta.io.file

File I/O modelled on Python's pathlib + Rust's `Path` + Go's
`os.ReadFile` — the common cases are one line, the edge cases don't fight
you. Every read / write / list / watch method carries
`@capability("filesystem")`; a program that doesn't declare the capability
fails at compile time (see BuildTool.md).

### `Path` — value type, immutable

```cajeta
public final class Path {
    // Construction
    public static Path of(String s);
    public static Path of(String... parts);          // joins as it builds
    public static Path of(byte[] os_bytes);          // raw OS-native bytes
    public static Path cwd();                        // process working dir
    public static Path home();                       // user home dir
    public static Path tempDir();                    // OS temp root

    // Joining — `/` is the syntactic sugar; `resolve` is the verbose form
    public Path operator/(String segment);
    public Path operator/(Path other);
    public Path resolve(String segment);

    // Decomposition
    public Path parent();                            // ".." semantics on root
    public String name();                            // last segment, with ext
    public String stem();                            // last segment, no ext
    public String extension();                       // ".tar.gz" -> "gz"
    public Array<String> parts();                    // split on separator
    public boolean isAbsolute();
    public boolean isRelative();

    // Normalisation
    public Path absolute();                          // resolve vs cwd()
    public Path canonical();                         // resolves symlinks too
    public Path normalize();                         // collapses "." / ".."
    public Path relativeTo(Path base);               // throws if not under

    // Predicates (single stat call each)
    public boolean exists();
    public boolean isFile();
    public boolean isDir();
    public boolean isSymlink();

    // Metadata — one batched stat, returned as a value
    public FileInfo info();                          // size + times + perms
}
```

Path is one type, not split into `AbsolutePath` / `RelativePath`. The
split adds boilerplate at every API boundary and is rarely load-bearing
in practice — `isAbsolute()` covers the few places it matters.

### `FileInfo` — batched stat result

```cajeta
public final value class FileInfo {
    public int64 size();
    public Instant created();
    public Instant modified();
    public Instant accessed();
    public boolean isFile();
    public boolean isDir();
    public boolean isSymlink();
    public int32 permissions();                      // POSIX mode bits
}
```

Returned by `Path.info()`. Callers that want one field still pay one
stat; callers that want six fields also pay one stat. Snapshots, so two
reads of `modified()` give the same answer.

### Reading and writing — the one-liners

```cajeta
// Whole-file reads
String text = Path.of("data.json").readText();              // UTF-8
String text = Path.of("data.json").readText(Encoding.LATIN1);
byte[]  bin = Path.of("model.bin").readBytes();

// Streaming reads
for (String line in Path.of("huge.log").readLines()) {
    process(line);
}
for (byte[] chunk in Path.of("video.mp4").readChunks(64 * 1024)) {
    pump(chunk);
}

// Whole-file writes — atomic by default (see below)
Path.of("output.txt").writeText("hello world");
Path.of("output.txt").writeText(s, Encoding.UTF_16);
Path.of("blob.bin").writeBytes(bytes);

// Appends — non-atomic by definition
Path.of("audit.log").appendText("entry\n");
Path.of("audit.bin").appendBytes(bytes);

// Filesystem ops
Path.of("a.txt").copyTo(Path.of("b.txt"));
Path.of("a.txt").moveTo(Path.of("b.txt"));               // atomic on same FS
Path.of("a.txt").delete();
Path.of("a.txt").deleteIfExists();
```

### `File` — when you need a handle (random access, partial reads)

```cajeta
public enum OpenMode { READ, WRITE, APPEND, READ_WRITE, CREATE_NEW }

public final class File implements InputStream, OutputStream {
    public static File open(Path p, OpenMode mode);
    public static File openExclusive(Path p);            // O_CREAT | O_EXCL

    public int64 read(Buffer dst);                       // returns bytes read
    public int64 write(Buffer src);                      // returns bytes written
    public int64 read(byte[] dst, int64 offset, int64 length);
    public int64 write(byte[] src, int64 offset, int64 length);

    public int64 position();
    public void  seek(int64 absolute);
    public void  seekFromEnd(int64 offset);
    public int64 size();
    public void  truncate(int64 size);
    public void  flush();                                // userspace flush
    public void  sync();                                 // fsync to disk
    public void  lock();                                 // advisory, blocking
    public boolean tryLock();
    public void  unlock();

    public void  close();                                // also called by drop
}
```

`File` implements `InputStream` and `OutputStream`, so anything that takes
a stream takes a file. The handle drops automatically when its owner
scope exits — explicit `close()` is for the rare case you need to release
before the scope ends.

### Directories: children, walk, glob, mkdirs

```cajeta
// One level deep
for (Path child in Path.of(".").children()) { ... }

// Recursive (DFS by default; .bfs() for breadth-first)
for (Path p in Path.of("src").walk()) { ... }
for (Path p in Path.of("src").walk().bfs()) { ... }

// Glob — `*` within a segment, `**` for any depth
for (Path p in Path.of("src").glob("**/*.cajeta")) { ... }
for (Path p in Path.of("logs").glob("2026-??-??.log")) { ... }

// Create directories
Path.of("a/b/c").mkdirs();                               // -p semantics
Path.of("a").mkdir();                                    // single level
```

`children()` / `walk()` / `glob()` return `Iterator<Path>` — they stream,
they don't materialise a list. For huge trees this is the only sensible
shape; if you want a list, `.toArray()` it.

### Atomic writes — what "by default" means

`writeText` / `writeBytes` are atomic from the reader's perspective:
the file at the target path either contains the old content or the new
content, never a half-written mix. Implementation: write to
`<name>.tmp.<rand>` in the same directory, `fsync` it, `rename` over the
target. Same-directory rename is atomic on every POSIX filesystem and on
NTFS via `MoveFileEx(..., REPLACE_EXISTING | WRITE_THROUGH)`.

```cajeta
Path.of("config.json").writeText(json);                  // atomic
Path.of("config.json").writeTextDirect(json);            // not atomic — opt out
```

The direct form exists for filesystems that can't rename (some FUSE
mounts, some network filesystems) or for performance when you genuinely
don't care.

### Watching the filesystem — `Watcher`

```cajeta
public enum WatchKind { CREATE, MODIFY, DELETE, RENAME }

public final value class FileEvent {
    public Path      path();
    public WatchKind kind();
    public Instant   timestamp();
    public Path      renameTarget();                     // null unless RENAME
}

public final class Watcher {
    public Iterator<FileEvent> events();                 // blocks fiber on read
    public void close();
}

public final value class WatchOptions {
    public WatchOptions recursive(boolean r);
    public WatchOptions debounce(Duration window);       // coalesces bursts
    public WatchOptions kinds(WatchKind... only);
}
```

```cajeta
Watcher w = Path.of("./config").watch(
    WatchOptions().recursive(true).debounce(Duration.millis(100)));
for (FileEvent e in w.events()) {
    reload(e.path());
}
```

Backed by `inotify` on Linux, `FSEvents` on macOS, `ReadDirectoryChangesW`
on Windows. The fiber parks on the underlying handle — no polling thread.

### Async forms

Every blocking method has a `*Async` form returning `Task<T>`:

```cajeta
Task<String>  t1 = Path.of("data.json").readTextAsync();
Task<byte[]>  t2 = Path.of("blob.bin").readBytesAsync();
Task<void>    t3 = Path.of("out.txt").writeTextAsync("...");
Task<FileInfo> t4 = Path.of("x").infoAsync();
```

The sync forms park the calling fiber on the reactor — they are
**not** blocking the OS thread. The async forms exist for callers that
want to launch many I/Os in parallel without one fiber per call (e.g.
"prefetch these 200 files, await all").

### Errors

```cajeta
public class IoException             extends RecoverableException;
public class NotFoundException       extends IoException;
public class PermissionException     extends IoException;
public class AlreadyExistsException  extends IoException;
public class IsDirectoryException    extends IoException;
public class NotDirectoryException   extends IoException;
public class CrossDeviceException    extends IoException;       // rename across FS
public class DiskFullException       extends IoException;
```

Exceptions, not `Result<T>`. Consistent with the rest of the stdlib (see
cajeta.error). Errors here are almost always programmer errors or
environment failures — both want stack traces and the usual `try` /
unwind path, not pervasive `?` propagation.

### Capability gating

Every method that touches the filesystem carries
`@capability("filesystem")`:

```cajeta
public final class Path {
    @capability("filesystem")
    public String readText();

    @capability("filesystem")
    public void writeText(String content);

    // ... etc. for every I/O-effecting method
}
```

A program whose manifest doesn't declare `"filesystem"` in its
`capabilities` array fails at compile time the moment it touches one of
these methods. Pure path-manipulation methods (`parent`, `name`,
`operator/`, `normalize`) are **not** gated — they're string operations
on a value type. See BuildTool.md for the capability flag list.

---

## cajeta.io.net

Lands with the fiber reactor / server harness. Surface sketch:

```cajeta
public interface Address { }
public final value class InetAddress4 implements Address { ... }
public final value class InetAddress6 implements Address { ... }
public final value class SocketAddress { Address host; int32 port; ... }

public interface Socket extends InputStream, OutputStream {
    public SocketAddress localAddress();
    public SocketAddress remoteAddress();
    public void close();
}

public interface ServerSocket {
    public Socket accept();                              // fiber-parks
    public SocketAddress localAddress();
    public void close();
}

public final class Selector { ... }                      // reactor surface
```

All concrete types live in the protocol-specific subpackages below.
Listed here so generic code (a TLS wrapper, an HTTP client, a proxy) can
program against `Socket` / `ServerSocket` without caring which protocol
backs it.

### cajeta.io.net.tcp

```cajeta
public final class TcpSocket implements Socket { ... }
public final class TcpServerSocket implements ServerSocket { ... }
public final class TcpListener { ... }                   // listen / bind config
```

### cajeta.io.net.udp

```cajeta
public final class UdpSocket { ... }
public final value class DatagramPacket { ... }
```

### cajeta.io.net.tls

```cajeta
public final class TlsContext { ... }                    // certs, ciphers
public final class TlsSocket implements Socket { ... }   // wraps another Socket
public final value class Certificate { ... }
public final value class PrivateKey { ... }
```

### cajeta.io.net.http

Full design in CajetaHttp.md. Top-level surface: `HttpClient`,
`HttpRequest`, `HttpResponse`, `HttpServer`, `Route`, `ServerMode`
(FIBER_PER_CONNECTION / EVENT_DRIVEN / HYBRID).

### cajeta.io.net.websocket

Full design in CajetaHttp.md. Top-level surface: `WebSocketClient`,
`WebSocketServer`, `WebSocket`, `Frame`. Uses the HTTP upgrade handshake
from cajeta.io.net.http.

---

## cajeta.process

Subprocess management. Sibling of `cajeta.io` and `cajeta.thread`, not
nested under either — a subprocess is its own concern (fork/exec
lifecycle, signal model, exit status). Stdin / stdout / stderr stream
through the same
`InputStream` / `OutputStream` interfaces used everywhere else, so the
pumps are fiber-aware automatically.

```cajeta
public enum Stdio { INHERIT, PIPE, NULL, FILE }

public final class ProcessBuilder {
    public ProcessBuilder(String command);
    public ProcessBuilder arg(String s);
    public ProcessBuilder args(String... s);
    public ProcessBuilder env(String key, String value);
    public ProcessBuilder cwd(Path dir);
    public ProcessBuilder stdin(Stdio mode);
    public ProcessBuilder stdout(Stdio mode);
    public ProcessBuilder stderr(Stdio mode);
    @capability("process")
    public Process start();
}

public final class Process {
    public int32  pid();
    public OutputStream stdin();                         // if Stdio.PIPE
    public InputStream  stdout();                        // if Stdio.PIPE
    public InputStream  stderr();                        // if Stdio.PIPE
    public ExitStatus   waitFor();                       // fiber-parks
    public ExitStatus   waitFor(Duration timeout);
    public void         terminate();                     // SIGTERM
    public void         kill();                          // SIGKILL
    public void         signal(Signal sig);
}

public final value class ExitStatus {
    public int32 code();
    public boolean success();
    public Signal terminatedBy();                        // null if exited normally
}
```

`@capability("process")` gates `start()` — running subprocesses is a
distinct capability from filesystem or network access.

---

## cajeta.thread

Documented separately in docs/ThreadModel.md. Stub here:

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
8. **cajeta.io.file: Path, FileInfo, readText / writeBytes / glob / walk.**
   Sync surface first; sync methods park on the reactor so async forms
   are a thin wrapper that lands later. Watcher is its own step (needs
   per-OS backends) — defer until something actually wants it.
9. **cajeta.time value types.** Instant first, then Duration, then the
   Local* types, then ZoneId / ZonedDateTime / DateTimeFormatter.
10. **cajeta.collection: TreeMap, TreeSet, DenseMap/Set, Heap, immutable
    variants, trees.** Less common; ship as needed.
11. **cajeta.io.net + .tcp + .tls.** Reactor surface and the protocol-
    specific concrete sockets. Unblocks cajeta.io.net.http /
    cajeta.io.net.websocket (which have their own design in CajetaHttp.md).
12. **cajeta.process.** ProcessBuilder + fiber-aware stream pumps.

Steps 1-6 unblock the server harness. Steps 7-12 fill out the library.

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
