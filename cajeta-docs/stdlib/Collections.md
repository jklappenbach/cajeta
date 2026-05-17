# `cajeta.collection` — Lists, Sets, Maps, and friends

The container types. Each is designed to multiple-inherit
`cajeta.lang.stream.Stream<E>` of the appropriate element type so the
container IS a stream — no `.iterator()` factory needed. See
Streams.md for the full stream protocol and combinator surface.

## Status snapshot

| Type | Status |
|------|--------|
| `ArrayList<T>` | shipped (with `.stream()` factory; multi-inheritance from Stream not yet) |
| `HashMap<K, V>` | shipped (no Stream surface yet) |
| `HashSet<T>` | designed, not implemented |
| `LinkedList<T>` | designed, not implemented |
| `Deque<T>` / `Stack<T>` | designed, not implemented |
| `Heap<T>` (priority queue) | designed, not implemented |
| Tree types (`TreeMap`, `TreeSet`, `BTreeMap`, …) | designed, not implemented |
| `Collector<T, R>` + `Collectors` | designed, not implemented |

## `ArrayList<T>` — shipped

Dynamic array. Heap-allocated `T[]` that grows on demand. Source at
`runtime/src/cajeta/collection/ArrayList.cajeta`.

```cajeta
public class ArrayList<T> {
    public ArrayList();                       // initial capacity 16
    public int32 size();
    public boolean isEmpty();
    public T get(int32 i);
    public void set(int32 i, T v);
    public void add(T v);                     // amortized O(1)
    public ArrayStream<T> stream();           // returns Stream<T>
}
```

### Examples

```cajeta
import cajeta.collection.ArrayList;

ArrayList<int32> list = heap ArrayList<int32>();
list.add(10);
list.add(20);
list.add(30);

int32 n = list.size();                        // 3
int32 first = list.get(0);                    // 10
list.set(1, 200);

// Stream pipeline over the live elements.
int32 sum = list.stream().reduce(0,
    (int32 a, int32 b) -> { return a + b; });
```

Pinned by `test/parser/ArrayListTests.cpp` (7 tests).

### Open items

- Multiple-inherit `Stream<T>` so `for (v : list)` works without the
  `.stream()` factory.
- `remove(int32 i)`, `clear()`, `contains(T v)`, `indexOf(T v)`.
- Bounds checks at `get`/`set`/`remove` once error model wiring is
  ready.

## `HashMap<K, V>` — shipped

Open-addressing hash map with linear probing + tombstones. Source at
`runtime/src/cajeta/collection/HashMap.cajeta`.

```cajeta
public class HashMap<K, V> {
    public HashMap(int64 initialCapacity);
    public void put(K key, V value);
    public V get(K key);                       // returns 0/null on miss
    public boolean containsKey(K key);
    public boolean remove(K key);
    public int64 count();

    public V operator[](K key);                // sugar for get
    public void operator[]=(K key, V value);   // sugar for put
}
```

K is constrained to class types (or primitive types with the
primitive-hash intrinsics) because the bucket index uses `key.hash()`
and bucket equality uses `==`.

Capacity is power-of-2; auto-grows past 0.75 load factor with
tombstone compaction on each resize.

### Examples

```cajeta
import cajeta.collection.HashMap;

HashMap<String, int32> counts = heap HashMap<String, int32>(64);
counts.put("apples", 3);
counts.put("oranges", 5);

int32 apples = counts.get("apples");          // 3
boolean has = counts.containsKey("kiwis");    // false

// Operator overloads
counts["grapes"] = 7;
int32 grapes = counts["grapes"];

// Iteration: requires the planned multiple-inheritance from
// Stream<Pair<K, V>>; not yet shipped.
```

Pinned by `test/collections/HashMapTests.cpp` and
`test/collections/PrimitiveHashMapTests.cpp`.

### Open items

- Multiple-inherit `Stream<Pair<K, V>>` so the map IS its entry-
  stream.
- `keys()` / `values()` projections (lazy streams over the underlying
  arrays).
- `getOrDefault(K, V)`, `putIfAbsent(K, V)`, `merge`.

## `HashSet<T>` — designed

Thin wrapper over `HashMap<T, Unit>` (or a dedicated lower-overhead
open-address table). Same key constraint as `HashMap`.

```cajeta
public class HashSet<T> {
    public HashSet();
    public HashSet(int64 initialCapacity);
    public boolean add(T v);                   // true if new
    public boolean contains(T v);
    public boolean remove(T v);
    public int64 count();
}
```

Multiple-inherits `Stream<T>` for iteration.

## `LinkedList<T>` — designed

Doubly-linked list with head/tail pointers.

```cajeta
public class LinkedList<T> {
    public LinkedList();
    public int64 count();
    public T head();                           // O(1)
    public T tail();                           // O(1)
    public void addFirst(T v);
    public void addLast(T v);
    public T removeFirst();
    public T removeLast();
}
```

Multiple-inherits `Stream<T>`.

## `Deque<T>` / `Stack<T>` — designed

`Deque<T>` is a double-ended queue; `Stack<T>` exposes the LIFO
subset (`push` / `pop` / `peek`). Both back onto a circular array
buffer that grows on demand.

```cajeta
public class Deque<T> {
    public void pushFront(T v);
    public void pushBack(T v);
    public T    popFront();
    public T    popBack();
    public T    peekFront();
    public T    peekBack();
    public int64 count();
}

public class Stack<T> {
    public void push(T v);
    public T    pop();
    public T    peek();
    public int64 count();
}
```

Both multiple-inherit `Stream<T>` (Deque in front-to-back order;
Stack in pop-order, top-to-bottom).

## `Heap<T>` — designed

Binary min-heap (or max-heap via a comparator).

```cajeta
public class Heap<T> {
    public Heap((T, T) -> int32 compare);
    public void push(T v);
    public T pop();                            // minimum
    public T peek();
    public int64 count();
}
```

Multiple-inherits `Stream<T>` (yield order = pop order).

## Tree types — designed (TreeMap, TreeSet, BTreeMap, BTreeSet)

Ordered map / set. `TreeMap` keys carry the bound
`K extends Comparable<K>` (the only place in the v1 stdlib where a
`Comparable` bound is required). BTreeMap is a sorted-by-key variant
backed by a cache-friendly B-tree node layout for large keyspaces.

```cajeta
public class TreeMap<K extends Comparable<K>, V> {
    public void put(K key, V value);
    public V get(K key);
    public Optional<K> firstKey();
    public Optional<K> lastKey();
    public Stream<Pair<K, V>> range(K fromInclusive, K toExclusive);
}
```

## `Collector<T, R>` and `Collectors` — designed

Terminal that materializes a stream into something else (a list, a
map, a count). The contract is the standard supplier/accumulator/
finisher triple:

```cajeta
public interface Collector<T, R> {
    public R supply();                         // initial accumulator
    public void accumulate(R acc, T element);
    public R finish(R acc);
}

public final class Collectors {
    public static Collector<T, ArrayList<T>> toList();
    public static Collector<T, HashSet<T>> toSet();
    public static Collector<T, HashMap<K, V>> toMap(...);
    public static Collector<T, int64> counting();
    public static Collector<T, int64> summingInt32((T) -> int32 fn);
    public static Collector<T, String> joining(String sep);
}
```

Used at the terminal end of a stream chain:

```cajeta
ArrayList<int32> evens = xs.stream()
    .filter(isEven)
    .collect(Collectors.toList());
```

Blocked on `Stream.collect(Collector<T, R>)` and on the chained-form
parsing (P6.6).

## For-loop desugaring through Stream

The `for (v : iterable)` form lowers to `iterable.next()` walking
calls when `iterable` is-a (multiple-inherits) `Stream<T>`. This is
the eventual story for HashMap / HashSet / LinkedList iteration —
the multiple-inheritance design eliminates the need for a separate
`Iterator<T>` abstraction.

Today the for-each loop works over arrays directly
(`for (int32 x : xs)`) via the array-iteration intrinsic; the
stream-receiver lowering is the next step.

## Open items

Tracked in Features.md:

- Collection types listed as "designed" above.
- Multiple-inheritance from `Stream<E>` on `ArrayList`, `HashMap`
  (and `HashSet` / `LinkedList` / etc. when they land).
- `Collector<T, R>` interface + `Collectors` factories +
  `Stream.collect`.
- For-loop desugaring for `Stream`-typed receivers (P6.x).
- Chained-form parsing (P6.6) so `xs.stream().filter(p).map(f).count()`
  works without intermediate locals.
