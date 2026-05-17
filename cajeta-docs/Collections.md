# Collections — Stream, Optional, Pair, and the Collection Library

This document specifies cajeta's iteration and collection types: the `Stream<T>` abstract base that carries the combinator vocabulary, the cardinality-1 type `Optional<T>`, the key/value tuple `Pair<K,V>`, the concrete collection classes (`ArrayList`, `HashMap`, `HashSet`, `LinkedList`), the `Collector<T,R>` protocol for materializing streams into containers, and the for-loop desugaring that ties them together.

This rollout lands **after** the unified-class rollout (`UnifiedClasses.md`) plus the supporting compiler features (definite-assignment analysis, covariant return types, live-borrow tracker for iterator invalidation). Every class in this document is a v2 unified class — single `class` keyword, `heap` or `stack` allocation at the use site, multi-inheritance via `extends`.

---

## Goals

- **One iteration protocol.** A single `Stream<T>` abstract class is the only thing implementing the pull protocol (`next() → Optional<T>`). No separate `Iterator<T>` / `Iterable<T>` / `Stream<T>` triple — that exists in other languages only for backward compatibility we don't have.
- **Rich combinator vocabulary inherited via abstract class.** ~25 lazy/eager ops on `Stream<T>` so every implementer gets them free. Multi-inheritance lets subclasses also inherit unrelated protocols (Hashable, Comparable, etc.) without burning the inheritance slot.
- **Optional as cardinality-1 stream.** `Optional<T>` extends `Stream<T>` with single-shot `next()`. Caller gets Java's familiar Optional surface (map, filter, flatMap with covariant `Optional<U>` returns) plus the full Stream vocabulary inherited.
- **Stack-resident pipelines.** Combinator chains are `stack`-allocated wrappers. Concrete-receiver chains monomorphize + inline to single tight loops via LLVM. Zero-cost abstraction in the Rust sense.
- **Collections produce streams via `.stream()`.** Collection classes don't extend `Stream` directly (avoids the cursor-trampling problem); each provides a fresh stream walking its storage. `list.stream().map(fn).filter(p).collect(...)` is the canonical chain.
- **Iterator invalidation caught at compile time.** A live read-borrow into a collection blocks any mutating call to that collection. Enabled by the live-borrow tracker (Phase 6 of the unified-class rollout).

---

## Non-goals (v1)

- **Async streams.** Out of scope; lands separately when the fiber runtime matures (`ThreadModel.md`, `AsyncStatus.md`).
- **Parallel streams.** No `.parallel()` flag (universally regretted in Java). Parallel iteration integrates with the fiber pool via an explicit `coll.parStream()` entry when added; not v1.
- **Reactive streams / backpressure / error channels.** Those are async concerns.
- **Sorted / tree-based collections.** `TreeMap`, `TreeSet`, `SortedSet` — wanted but defer; v1 is hash-based and array-based collections.
- **Concurrent collections.** No `ConcurrentHashMap` equivalent; safe-share semantics belong with the fiber runtime work.
- **Persistent / immutable collections.** Java's `List.of()` / `Map.of()` style. Useful but defer; v1 is mutable collections.

---

## Type map at a glance

| Type | Role | Storage convention |
|---|---|---|
| `Stream<T>` (abstract) | Pull protocol + combinator vocabulary | `stack` by default; combinator wrappers are stack-resident |
| `Optional<T>` | Cardinality-1 value-or-none | `stack` for locals/returns (RVO); `heap` only when escape needed |
| `Pair<K,V>` | Two-field generic value | Same convention as Optional |
| `ArrayStream<T>` | Walks a `T[]` | `stack` (compiler intrinsic via `arr.stream()`) |
| `MapStream<T,U>`, `FilterStream<T>`, etc. | Lazy combinator wrappers | `stack` |
| `ArrayList<T>` | Dynamic array; appends, indexed access | `stack` or `heap` per use |
| `HashSet<T>` | Hash-backed set | Same |
| `HashMap<K,V>` | Hash-backed map | Same |
| `LinkedList<T>` | Doubly-linked list | Same |
| `HashMapEntryStream<K,V>`, `HashMapKeyStream<K,V>`, `HashMapValueStream<K,V>` | Per-axis iteration | `stack` (returned by HashMap methods) |
| `Collector<T,R>` (abstract) | Reduces a stream into a container | `stack` typically |
| `Collectors` | Static methods returning common collectors | static; no instances |

---

## `Pair<K,V>`

Foundation building block. Two fields, two accessors, aggregate-init friendly.

```cajeta
package cajeta.lang;

public class Pair<K, V> {
    K first;
    V second;

    public Pair(K first, V second) {
        this.first = first;
        this.second = second;
    }

    public K first()  { return this.first; }
    public V second() { return this.second; }
}

// Use:
Pair<String, int32> p = stack Pair("count", 42);
Pair<String, int32> q = stack Pair { first: "count", second: 42 };  // aggregate-init
```

Lives in `cajeta.lang` because it's used across the stdlib (HashMap entries, Stream.zip results, etc.).

---

## `Optional<T>`

Value-typed sum: present or empty. Stack-allocated by default. Implements `Stream<T>` with single-shot `next()` — yields the value once, then exhausted.

### Layout

```cajeta
public class Optional<T> extends Stream<T> {
    boolean present;
    T       value;

    // ... methods below
}
```

`{boolean present; T value;}` — one byte plus T-sized payload. `T` may be a primitive, a class reference, another struct, or another Optional. No heap allocation; lives in whatever frame holds it.

### Construction

```cajeta
public static Optional<T> Some(T v) {
    return stack Optional { present: true, value: v };
}
public static Optional<T> None() {
    return stack Optional { present: false, value: /* zero-init */ };
}
```

Returns travel via the caller's RVO/sret slot — no heap allocation, no escape error.

Aggregate-init is also legal: `stack Optional { present: true, value: 42 }`. Factories are the conventional surface.

### Inspection

```cajeta
public boolean isPresent() { return this.present; }
public boolean isEmpty()   { return !this.present; }
```

### Extraction

```cajeta
public T get() {
    if (!this.present) throw stack NoSuchElementException("Optional.get on None");
    return this.value;
}

public T orElse(T fallback) {
    return this.present ? this.value : fallback;
}

public T orElseGet(() -> T producer) {
    return this.present ? this.value : producer();
}

public T orElseThrow(Exception e) {
    if (this.present) return this.value;
    throw e;
}

public T orElseThrow(() -> Exception producer) {
    if (this.present) return this.value;
    throw producer();
}
```

`orElseThrow(() -> Exception)` is the catch-all for "unwrap with custom error handling" — side effects (logging, metrics) happen inside the lambda before returning the exception. (Java's `expect(msg)` / Rust's `expect(&str)` collapse into this one method.)

### Transformation (cardinality-1-preserving overrides)

```cajeta
@Override
public <U> Optional<U> map((T) -> U fn) {
    return this.present
        ? Optional<U>.Some(fn(this.value))
        : Optional<U>.None();
}

@Override
public Optional<T> filter((T) -> boolean p) {
    return (this.present && p(this.value)) ? this : Optional<T>.None();
}

@Override
public <U> Optional<U> flatMap((T) -> Optional<U> fn) {
    return this.present ? fn(this.value) : Optional<U>.None();
}
```

These **override** the inherited Stream versions with covariant `Optional<U>` return types. At a concrete-Optional call site, the result is `Optional<U>`; at a `Stream<T>` call site, the result is `Stream<U>`.

### Optional<T> only — not on Stream

```cajeta
public Optional<T> or(() -> Optional<T> alt) {
    return this.present ? this : alt();
}

public <R> R fold(() -> R ifNone, (T) -> R ifSome) {
    return this.present ? ifSome(this.value) : ifNone();
}

public Optional<T> inspect((T) -> void action) {
    if (this.present) action(this.value);
    return this;
}

public void ifPresent((T) -> void c) {
    if (this.present) c(this.value);
}

public void ifPresentOrElse((T) -> void onSome, () -> void onNone) {
    if (this.present) onSome(this.value); else onNone();
}
```

### Stream protocol — single-shot `next()`

```cajeta
@Override
public Optional<T> next() {
    if (!this.present) return Optional<T>.None();
    Optional<T> out = Optional<T>.Some(this.value);
    this.present = false;          // single-shot — subsequent next() returns None
    return out;
}
```

Calling `next()` mutates the Optional's `present` flag. After the first call, the Optional is exhausted. This makes `for (T x in opt) { ... }` run the body once (if present) or zero times (if empty), naturally.

The Stream combinators (count, fold, forEach, collect, take, skip, ...) all inherited from Stream — work on Optional uniformly.

### Methods inherited from Stream<T>

Every combinator on Stream<T> works on Optional<T>. `opt.count()` returns 1 or 0; `opt.collect(toList())` returns a 0-or-1 element list; `opt.forEach(fn)` runs the function 0 or 1 times. All for free via inheritance.

---

## `Stream<T>`

The abstract base for everything that yields elements via a pull protocol. Single abstract method (`next()`); concrete combinator implementations built on top.

### Pull primitive

```cajeta
package cajeta.lang;

public abstract class Stream<T> {
    public abstract Optional<T> next();
    // ... combinators below
}
```

`next()` returns `Optional<T>.Some(v)` for the next element or `Optional<T>.None()` once exhausted. Once `None` is returned, subsequent `next()` calls also return `None` (well-behaved streams; not a hard requirement, but combinator behavior assumes it).

### Intermediate combinators (lazy)

Each returns a new stack-allocated stream wrapper. No work happens until something pulls.

```cajeta
public <U> Stream<U> map((T) -> U fn) {
    return stack MapStream<T,U> { source: this, fn: fn };
}
public Stream<T> filter((T) -> boolean p) {
    return stack FilterStream<T> { source: this, pred: p };
}
public <U> Stream<U> flatMap((T) -> Stream<U> fn) {
    return stack FlatMapStream<T,U> { source: this, fn: fn, current: null };
}
public Stream<T> take(int32 n) {
    return stack TakeStream<T> { source: this, remaining: n };
}
public Stream<T> skip(int32 n) {
    return stack SkipStream<T> { source: this, toSkip: n };
}
public Stream<T> takeWhile((T) -> boolean p) {
    return stack TakeWhileStream<T> { source: this, pred: p, done: false };
}
public Stream<T> dropWhile((T) -> boolean p) {
    return stack DropWhileStream<T> { source: this, pred: p, dropping: true };
}
public Stream<T> peek((T) -> void fn) {
    return stack PeekStream<T> { source: this, fn: fn };
}
public Stream<T> distinct() {
    return stack DistinctStream<T> { source: this, seen: stack HashSet<T>() };
}
public Stream<Pair<int32,T>> enumerate() {
    return stack EnumerateStream<T> { source: this, idx: 0 };
}
public <U> Stream<Pair<T,U>> zip(Stream<U> other) {
    return stack ZipStream<T,U> { left: this, right: other };
}
public Stream<T> chain(Stream<T> next) {
    return stack ChainStream<T> { left: this, right: next, onFirst: true };
}
public Stream<T> concat(Stream<T> next) {
    return this.chain(next);   // alias
}
public Stream<T> sorted((T, T) -> int32 cmp) {
    return stack SortedStream<T> { source: this, cmp: cmp, materialized: null };
}
public Stream<T[]> windowed(int32 size) {
    return stack WindowedStream<T> { source: this, size: size, window: null };
}
```

### Terminal combinators (eager)

Each calls `next()` until exhausted.

```cajeta
public void forEach((T) -> void fn) {
    Optional<T> o = this.next();
    while (o.isPresent()) { fn(o.get()); o = this.next(); }
}

public <R> R fold(R init, (R, T) -> R combiner) {
    R acc = init;
    Optional<T> o = this.next();
    while (o.isPresent()) { acc = combiner(acc, o.get()); o = this.next(); }
    return acc;
}

public Optional<T> reduce((T, T) -> T combiner) {
    Optional<T> first = this.next();
    if (!first.isPresent()) return first;
    T acc = first.get();
    Optional<T> o = this.next();
    while (o.isPresent()) { acc = combiner(acc, o.get()); o = this.next(); }
    return Optional<T>.Some(acc);
}

public int32 count() {
    int32 n = 0;
    Optional<T> o = this.next();
    while (o.isPresent()) { n = n + 1; o = this.next(); }
    return n;
}

public T sum() {
    // requires T to be a numeric type; compile-time check via type constraint
    // (TBD: how cajeta expresses "T is numeric" — see Open questions)
    ...
}

public Optional<T> findFirst() { return this.next(); }

public boolean anyMatch((T) -> boolean p) {
    Optional<T> o = this.next();
    while (o.isPresent()) { if (p(o.get())) return true; o = this.next(); }
    return false;
}

public boolean allMatch((T) -> boolean p) {
    Optional<T> o = this.next();
    while (o.isPresent()) { if (!p(o.get())) return false; o = this.next(); }
    return true;
}

public boolean noneMatch((T) -> boolean p) { return !this.anyMatch(p); }

public T[] toArray() {
    ArrayList<T> tmp = stack ArrayList<T>();
    Optional<T> o = this.next();
    while (o.isPresent()) { tmp.add(o.get()); o = this.next(); }
    return tmp.toArray();
}

public <R> R collect(Collector<T, R> c) {
    R container = c.supply();
    Optional<T> o = this.next();
    while (o.isPresent()) { c.accumulate(container, o.get()); o = this.next(); }
    return c.finish(container);
}

public Map<K, ArrayList<T>> groupBy(<K> (T) -> K keyFn) {
    HashMap<K, ArrayList<T>> result = stack HashMap<K, ArrayList<T>>();
    Optional<T> o = this.next();
    while (o.isPresent()) {
        T v = o.get();
        K key = keyFn(v);
        if (!result.containsKey(key)) result.put(key, stack ArrayList<T>());
        result.get(key).add(v);
        o = this.next();
    }
    return result;
}
```

### Concrete stream-wrapper classes

Each intermediate combinator's return type:

```cajeta
public class MapStream<T,U> extends Stream<U> {
    Stream<T> source;
    (T) -> U  fn;
    @Override
    public Optional<U> next() {
        Optional<T> src = this.source.next();
        if (!src.isPresent()) return Optional<U>.None();
        return Optional<U>.Some(this.fn(src.get()));
    }
}

public class FilterStream<T> extends Stream<T> {
    Stream<T>       source;
    (T) -> boolean  pred;
    @Override
    public Optional<T> next() {
        while (true) {
            Optional<T> src = this.source.next();
            if (!src.isPresent()) return Optional<T>.None();
            T v = src.get();
            if (this.pred(v)) return Optional<T>.Some(v);
        }
    }
}

public class TakeStream<T> extends Stream<T> {
    Stream<T> source;
    int32     remaining;
    @Override
    public Optional<T> next() {
        if (this.remaining <= 0) return Optional<T>.None();
        this.remaining = this.remaining - 1;
        return this.source.next();
    }
}

// ... SkipStream, TakeWhileStream, DropWhileStream, PeekStream, DistinctStream,
//     EnumerateStream, ZipStream, ChainStream, SortedStream, WindowedStream,
//     FlatMapStream, etc. — same pattern, each wraps source + operation params,
//     implements next().
```

Stack-allocated, monomorphized at compile time when receivers are concrete, inlined by LLVM. The wrapper chain collapses to a single tight loop in the emitted IR — zero-cost abstraction.

---

## `ArrayStream<T>` and the `T[].stream()` intrinsic

`T[]` is a primitive cajeta type. Its `.stream()` method is a **compiler intrinsic**: the visitor recognizes `arr.stream()` as a special form and lowers it to a freshly-allocated `ArrayStream<T>` walking the array.

```cajeta
public class ArrayStream<T> extends Stream<T> {
    int32 idx;
    int32 limit;
    T[]   data;

    @Override
    public Optional<T> next() {
        if (this.idx >= this.limit) return Optional<T>.None();
        T v = this.data[this.idx];
        this.idx = this.idx + 1;
        return Optional<T>.Some(v);
    }
}

// Use:
int32[] nums = { 1, 2, 3, 4, 5 };
int32 sum = nums.stream().fold(0, (acc, x) -> acc + x);   // 15
```

The intrinsic exists so the for-loop desugaring and the array-streaming idiom don't need a stdlib roundtrip. `T[]` stays a primitive (no boxing); no `Array<T>` class lives in the stdlib.

---

## Collection types

All collections provide `.stream()` returning `Stream<T>`. None extend `Stream<T>` directly — collections shouldn't have iteration state. Each `.stream()` call returns a fresh stream; multiple concurrent streams over the same collection are fine (they each have their own cursor).

### `ArrayList<T>`

Dynamic array. Indexed access; append; remove; size.

```cajeta
public class ArrayList<T> {
    T[]   data;
    int32 size;
    int32 capacity;

    public ArrayList() { this.capacity = 16; this.data = new T[16]; this.size = 0; }
    public ArrayList(int32 initialCapacity) { ... }

    public int32 size()    { return this.size; }
    public boolean isEmpty() { return this.size == 0; }

    public T    get(int32 i)         { ... bounds check ... return this.data[i]; }
    public void set(int32 i, T v)    { ... bounds check ... this.data[i] = v; }
    public void add(T v)             { ... grow if needed ... }
    public void insert(int32 i, T v) { ... }
    public T    remove(int32 i)      { ... }
    public void clear()              { ... }

    public Stream<T> stream() {
        return stack ArrayStream<T> { idx: 0, limit: this.size, data: this.data };
    }

    public T[] toArray() { ... copies data[0..size] into a fresh array ... }
}
```

### `LinkedList<T>`

Doubly-linked. `add`, `addFirst`, `removeFirst`, `removeLast`.

```cajeta
public class LinkedList<T> {
    LinkedListNode<T> head;
    LinkedListNode<T> tail;
    int32             size;

    // standard linked-list methods

    public Stream<T> stream() {
        return stack LinkedListStream<T> { current: this.head };
    }
}

class LinkedListNode<T> { T value; LinkedListNode<T> next; LinkedListNode<T> prev; }

class LinkedListStream<T> extends Stream<T> {
    LinkedListNode<T> current;
    @Override
    public Optional<T> next() {
        if (this.current == null) return Optional<T>.None();
        T v = this.current.value;
        this.current = this.current.next;
        return Optional<T>.Some(v);
    }
}
```

### `HashSet<T>`

Hash-backed set. Add, contains, remove, size.

```cajeta
public class HashSet<T> {
    // internal hash storage
    public void    add(T v)        { ... }
    public boolean contains(T v)   { ... }
    public boolean remove(T v)     { ... }
    public int32   size()          { ... }
    public Stream<T> stream() { ... }
}
```

### `HashMap<K,V>`

Hash-backed map. Three iteration entry points — `entries`, `keys`, `values`. HashMap does **not** extend Stream directly because the iteration shape isn't single-axis.

```cajeta
public class HashMap<K, V> {
    // internal hash storage

    public V       get(K key)              { ... }
    public void    put(K key, V value)     { ... }
    public boolean containsKey(K key)      { ... }
    public V       remove(K key)           { ... }
    public int32   size()                  { ... }

    public Stream<Pair<K,V>> entries() {
        return stack HashMapEntryStream<K,V> { /* state */ };
    }
    public Stream<K> keys() {
        return stack HashMapKeyStream<K,V> { /* state */ };
    }
    public Stream<V> values() {
        return stack HashMapValueStream<K,V> { /* state */ };
    }
}

// Use:
HashMap<String, int32> counts = stack HashMap<String, int32>();
counts.put("a", 1); counts.put("b", 2);

for (Pair<String, int32> e in counts.entries()) {
    System.out.println(e.first() + " = " + e.second());
}

int32 total = counts.values().sum();
```

### Future collections (deferred)

- `TreeMap<K,V>` / `TreeSet<T>` — sorted; needs `Comparable<T>` infrastructure
- `Deque<T>` / `ArrayDeque<T>` — head/tail-mutable deque
- `PriorityQueue<T>` — heap-backed priority queue
- `ConcurrentHashMap<K,V>` — multi-fiber-safe; lands with fiber-safety primitives

---

## `Collector<T,R>` and `Collectors`

The polymorphic terminal `collect(Collector<T,R>)` reduces a stream into a container. Built-in collectors handle common cases; user-defined collectors slot in by implementing the abstract class.

### Protocol

```cajeta
package cajeta.lang;

public abstract class Collector<T, R> {
    public abstract R    supply();                              // create empty container
    public abstract void accumulate(R container, T element);    // add an element
    public R             finish(R container) { return container; }  // optional final transform
}
```

Three abstract phases match Java's pattern:
- `supply()` — produces an empty container of type `R`.
- `accumulate(c, e)` — folds element `e` into container `c`.
- `finish(c)` — optional final transformation (e.g., wrapping in immutable view, unboxing); defaults to identity.

### Built-in collectors

Live in `cajeta.lang.Collectors`. Static methods returning `Collector<T,R>` instances.

```cajeta
package cajeta.lang;

public class Collectors {
    public static <T> Collector<T, ArrayList<T>> toArrayList() {
        return stack ArrayListCollector<T>();
    }

    public static <T> Collector<T, LinkedList<T>> toLinkedList() {
        return stack LinkedListCollector<T>();
    }

    public static <T> Collector<T, HashSet<T>> toHashSet() {
        return stack HashSetCollector<T>();
    }

    public static <K,V> Collector<Pair<K,V>, HashMap<K,V>> toHashMap() {
        return stack HashMapCollector<K,V>();
    }

    public static <T> Collector<T, T[]> toArray() {
        return stack ArrayCollector<T>();
    }

    public static Collector<String, String> joining() {
        return stack JoiningCollector("");
    }
    public static Collector<String, String> joining(String separator) {
        return stack JoiningCollector(separator);
    }
    public static Collector<String, String> joining(String separator, String prefix, String suffix) {
        return stack JoiningCollector(separator, prefix, suffix);
    }

    public static <T> Collector<T, int64> counting() {
        return stack CountingCollector<T>();
    }

    public static Collector<int32, int64> summingInt32() {
        return stack SummingInt32Collector();
    }
    public static Collector<int64, int64> summingInt64() {
        return stack SummingInt64Collector();
    }

    public static <T, K> Collector<T, HashMap<K, ArrayList<T>>> groupingBy(<K> (T) -> K classifier) {
        return stack GroupingByCollector<T, K>(classifier);
    }

    public static <T> Collector<T, Pair<ArrayList<T>, ArrayList<T>>> partitioningBy((T) -> boolean p) {
        return stack PartitioningByCollector<T>(p);
    }
}
```

### Example concrete collector

```cajeta
class ArrayListCollector<T> extends Collector<T, ArrayList<T>> {
    @Override
    public ArrayList<T> supply() { return stack ArrayList<T>(); }
    @Override
    public void accumulate(ArrayList<T> c, T e) { c.add(e); }
    // finish() defaults to identity
}

class JoiningCollector extends Collector<String, String> {
    String separator;
    String prefix;
    String suffix;
    StringBuilder builder;
    boolean first;

    public JoiningCollector(String separator) { /* prefix and suffix empty */ ... }
    public JoiningCollector(String separator, String prefix, String suffix) { ... }

    @Override
    public String supply() {
        this.builder = stack StringBuilder();
        this.builder.append(this.prefix);
        this.first = true;
        return ""; // placeholder; real state in this.builder
    }
    @Override
    public void accumulate(String container, String e) {
        if (!this.first) this.builder.append(this.separator);
        this.builder.append(e);
        this.first = false;
    }
    @Override
    public String finish(String container) {
        this.builder.append(this.suffix);
        return this.builder.toString();
    }
}
```

### Use sites

```cajeta
ArrayList<int32> nums = ...;
HashSet<int32> evens = nums.stream()
    .filter(x -> x % 2 == 0)
    .collect(Collectors.toHashSet());

String csv = nums.stream()
    .map(x -> Integer.toString(x))
    .collect(Collectors.joining(", ", "[", "]"));

HashMap<String, ArrayList<User>> byCity = users.stream()
    .collect(Collectors.groupingBy(u -> u.city()));
```

---

## For-loop desugaring

`for (T x in stream)` lowers to `next()`-pumping over the stream. Works on any `Stream<T>` receiver — collections (via their `.stream()`), Optional (single-shot stream), or any custom Stream subclass.

```cajeta
// Source:
for (T x in someStream) {
    body;
}

// Desugaring:
{
    Stream<T> __it = someStream;
    Optional<T> __opt = __it.next();
    while (__opt.isPresent()) {
        T x = __opt.get();
        body;
        __opt = __it.next();
    }
}
```

For collections, the user calls `.stream()` explicitly:

```cajeta
ArrayList<int32> nums = ...;
for (int32 n in nums.stream()) {
    process(n);
}
```

For Optional, no `.stream()` call needed (Optional IS a Stream):

```cajeta
Optional<User> u = lookup(id);
for (User user in u) {
    render(user);   // runs once if present, zero times if not
}
```

For `T[]` arrays, the existing `for (T x : arr)` shorthand can be sugar for `arr.stream()`:

```cajeta
int32[] nums = { 1, 2, 3 };
for (int32 n : nums) { ... }   // equivalent to: for (int32 n in nums.stream()) { ... }
```

The existing `for (int32 i, T x : arr)` indexed-binding form continues to work (it predates Stream and stays as-is for arrays).

---

## Iterator invalidation

The live-borrow tracker (Phase 6 of the unified-class rollout — see `UnifiedClasses.md`) treats `coll.stream()` as a read-borrow of `coll`. While the stream is alive, any write through `coll`'s path is a compile-time error.

```cajeta
ArrayList<int32> nums = stack ArrayList<int32>();
nums.add(1); nums.add(2); nums.add(3);

for (int32 x in nums.stream()) {
    nums.add(99);            // COMPILE ERROR: CAJETA_ERROR_MUTATION_DURING_BORROW
    nums.set(0, x);          // also error — set is a mutating method on a live-borrowed receiver
    int32 other = nums.get(1);   // OK — read through a live read-borrow is fine
}
nums.add(99);                    // OK — borrow released at end of loop
```

The tracker is general — applies to any stream borrowing from any collection, not just for-loops. Streams stored in variables, passed as arguments, returned from functions all pin the source against mutation for their lifetime.

---

## Performance notes

### Stack-allocated pipelines are zero-cost when receivers are concrete

A chain like:

```cajeta
int32 result = nums.stream()
    .map(x -> x * 2)
    .filter(x -> x > 10)
    .take(5)
    .fold(0, (acc, x) -> acc + x);
```

Compiles (when `nums`'s concrete type is known and lambdas are non-capturing or capture-by-copy) to a single tight loop:
- `ArrayStream<int32>`, `MapStream<int32,int32>`, `FilterStream<int32>`, `TakeStream<int32>` are all stack-allocated.
- `Stream.fold` is monomorphized for the concrete chain type.
- LLVM inlines the `next()` calls through the chain.
- Final IR: a single loop over `nums.data` that doubles, checks predicate, counts, breaks at 5, accumulates.

No virtual calls. No heap allocation. Comparable to a hand-written loop. Same insight Rust's iterators leverage.

### Interface-typed stream receivers pay one vtable hop per `next()`

When the receiver type is `Stream<T>` (you've type-erased — say, passed the stream as a function argument typed `Stream<int32>`), each `next()` goes through the fat-pointer dispatch path (S9.5.6). One vtable hop per element. Still cheap; not zero-cost.

The common case (concrete-receiver pipelines built and consumed in one function) is zero-cost. The interface-typed case (passing streams across function boundaries with abstract types) pays a small per-element overhead.

### Boxing concerns

Cajeta's monomorphization avoids boxing for primitive type parameters. `Stream<int32>` doesn't box int32 into an Integer wrapper; the chain is statically specialized for int32 throughout. Java's `IntStream`/`LongStream`/`DoubleStream` exist specifically to avoid boxing; cajeta gets the same benefit through monomorphization, with no surface explosion.

---

## Implementation roadmap

The Collections rollout lands **after** the unified-class rollout (`UnifiedClasses.md`) and its supporting compiler features. Sessions sized comparable to S6-S11. Status tracking in `ToDo.md`.

### Phase 1 — Foundation types

- `cajeta.lang.Pair<K,V>` — two-field generic class, aggregate-init friendly.
- `cajeta.lang.Optional<T>` — value-typed sum; full method surface per § Optional; no Stream extension yet.
- Tests: construction, accessors, all extraction/transformation methods.

### Phase 2 — Stream abstract class and combinators

- `cajeta.lang.stream.Stream<T>` — abstract class with `next()` abstract + ~25 concrete combinators (per § Stream).
- Concrete combinator wrappers: `MapStream`, `FilterStream`, `FlatMapStream`, `TakeStream`, `SkipStream`, `TakeWhileStream`, `DropWhileStream`, `PeekStream`, `DistinctStream`, `EnumerateStream`, `ZipStream`, `ChainStream`, `SortedStream`, `WindowedStream`.
- Optional re-declared to `extends Stream<T>`; single-shot `next()`; covariant `map`/`filter`/`flatMap` overrides.
- Tests: combinator behavior, chaining, terminal correctness, Optional-as-Stream.

### Phase 3 — Array integration

- `T[].stream()` compiler intrinsic — visitor recognizes the call, lowers to `stack ArrayStream<T>`.
- `cajeta.lang.stream.ArrayStream<T>` (used internally; not directly user-instantiated typically).
- For-loop desugaring path 1: `for (T x : arr)` and `for (T x in arr.stream())` both work.
- Tests: streaming over arrays of all primitive types + class arrays.

### Phase 4 — Collector and Collectors

- `cajeta.lang.Collector<T,R>` — abstract class with `supply`/`accumulate`/`finish`.
- `cajeta.lang.Collectors` — static methods returning built-in collectors (toArrayList, toLinkedList, toHashSet, toHashMap, toArray, joining, counting, summingInt32, summingInt64, groupingBy, partitioningBy).
- `Stream.collect(Collector<T,R>)` terminal.
- Tests: each built-in collector + a user-defined collector example.

### Phase 5 — Concrete collections

- `cajeta.lang.ArrayList<T>` — full surface; `stream()` returns ArrayStream.
- `cajeta.lang.LinkedList<T>` — full surface; `stream()` returns LinkedListStream.
- `cajeta.lang.HashSet<T>` — full surface; `stream()` returns HashSetStream.
- `cajeta.lang.HashMap<K,V>` — full surface; `entries()` / `keys()` / `values()` return distinct streams.
- Tests: collection operations, iteration, stream-based operations on each.

### Phase 6 — For-loop desugaring through Stream

- Extend the visitor's for-loop lowering to recognize `for (T x in expr)` where `expr` is a `Stream<T>`.
- Tests: `for` over arrays, collections, Optional, custom user-defined streams.

### Phase 7 — Iterator invalidation tests

- The live-borrow tracker from Phase 6 of the unified-class rollout is already live; this phase adds the iteration-specific test coverage.
- Tests: `nums.stream()` blocks `nums.add(...)`; nested streams correctly track per-stream borrows; stream-out-of-scope releases the borrow.

---

## Open questions

- **`sum()` and numeric constraints.** How does cajeta express "T is a numeric type" for terminals like `sum()` / `average()`? Three options: per-primitive overloads (`int32 sumInt32()`, `double sumDouble()`), a `Numeric<T>` interface implemented by primitive boxings, or accept that `sum()` lives on `Collectors` (which can do per-type via the static method specializations). My lean: leave `sum()` off Stream; provide `Collectors.summingInt32()` etc. and let users do `stream.collect(Collectors.summingInt32())`. Matches Java's pattern.
- **Stream-of-streams flattening.** `flatMap` covers element-stream-of-element-stream. What about a stream-of-collections case (`Stream<ArrayList<T>>` to `Stream<T>`)? Probably just `s.flatMap(coll -> coll.stream())` — explicit. No special method.
- **Closeable streams.** Java has `Stream<T>` implement `AutoCloseable` for file-line and IO-backed streams. Cajeta's drop chain auto-closes when the stream goes out of scope (file-line stream's destructor releases the handle). No explicit close method needed; the drop chain handles it.
- **Stream re-use semantics.** A consumed stream returns `None` forever; calling combinators on an exhausted stream produces empty results. Documented as expected behavior — no explicit "stream is consumed" error. Matches Java's "Stream has already been operated upon or closed" but lazier (no runtime check; just empty results).

---

## Related documents

- `UnifiedClasses.md` — the v2 class model this rollout sits on top of.
- `MemoryModel.md` — borrow / move / drop doctrine; the live-borrow tracker that powers iterator-invalidation safety extends this.
- `StandardLibrary.md` — the broader stdlib spec; this document is a focused slice covering iteration + collections.
- `Views.md` — typed overlays onto byte buffers; not iteration-related, but the design philosophy (zero-copy, monomorphized, stack-resident) is shared.
- `Structs.md` — v1 struct spec; archive when the `struct` keyword retires.
- `ToDo.md` — working tracker; this document formalizes the sealed-decisions state for the iteration/collection slice.
