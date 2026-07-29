# `cajeta.collection` — Lists, Sets, Maps, and friends

The container types in `runtime/src/cajeta/collection/`. Today each
container exposes its elements as a `cajeta.lang.stream.Stream<E>`
through a factory method (`stream()`, or `keys()`/`values()`/`entries()`
on `HashMap`) rather than multiple-inheriting `Stream` directly; the
multiple-inheritance "container IS a stream" design is still pending.
See Streams.md for the full stream protocol and combinator surface.

All containers report their element count via `count()` (never `size`
or `length`); `count()` returns `int64` except on `ArrayList`, where it
is `int32`.

## Status snapshot

| Type | Status |
|------|--------|
| `ArrayList<T>` | shipped — `count`/`isEmpty`/`get`/`set`/`add`/`appendAll`/`stream` |
| `HashMap<K, V>` | shipped — incl. `keys()`/`values()`/`entries()` `Splittable` stream views |
| `HashSet<T>` | shipped — backed by `HashMap<T, _>` |
| `LinkedList<T>` | shipped — doubly-linked, head/tail ops |
| `Heap<T>` (priority queue) | shipped — binary min-heap, `<`-ordered |
| `ImmutableList<T>` / `ImmutableSet<T>` / `ImmutableMap<K, V>` | shipped — frozen snapshots with `.stream()` |
| `RedBlackTree<K, V>` (ordered map) | shipped — insert/lookup/min/max/`keysInOrder`; delete deferred |
| `BPlusTree<K, V>` (ordered map) | shipped — insert/lookup/min/`keysInOrder`; delete deferred |
| `Cache<K, V>` (LRU + TTL) | shipped — `get`/`put`/`evict`, age-bounded |
| `Collector<T, R>` + `Collectors` | shipped — `Collector` triple + `Collectors.toList<T>()` |
| `Deque<T>` / `Stack<T>` | designed, not implemented (no source yet) |
| `ltm.BPlusTree<K, V>` (larger-than-memory, disk-backed) | planned — see "Larger-than-memory" below |
| `TreeMap` / `TreeSet` / `BTreeMap` | not built — superseded by `RedBlackTree` / `BPlusTree` |

## `ArrayList<T>` — shipped

Dynamic array. Heap-allocated `T[]` that grows on demand. Source at
`runtime/src/cajeta/collection/ArrayList.cajeta`.

```cajeta
public class ArrayList<T> {
    public ArrayList();                       // initial capacity 16
    public int32 count();                     // element count
    public boolean isEmpty();
    public T get(int32 i);
    public void set(int32 i, T v);
    public void add(T v);                     // amortized O(1), capacity doubles
    public void appendAll(ArrayList<T> other);// append other's elements (non-consuming)
    public #ArrayStream<T> stream();          // owned Stream<T> over live elements
}
```

`appendAll` is the combiner used when parallel `Collectors.toList<T>()`
merges per-worker partial lists.

### Examples

```cajeta
import cajeta.collection.ArrayList;

ArrayList<int32> list = heap ArrayList<int32>();
list.add(10);
list.add(20);
list.add(30);

int32 n = list.count();                       // 3
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
    public HashMap(int64 initialCapacity);     // power-of-2 capacity
    public void put(K key, V value);
    public V get(K key);                        // returns 0/null on miss
    public boolean containsKey(K key);
    public boolean remove(K key);              // leaves a reusable tombstone
    public int64 count();

    public V operator[](K key);                // sugar for get
    public void operator[]=(K key, V value);   // sugar for put

    // Splittable stream views (snapshots; slot-walk order):
    public #Stream<K> keys();                  // HashMapKeyStream<K, V>
    public #Stream<V> values();                // HashMapValueStream<K, V>
    public #Stream<Pair<K, V>> entries();      // HashMapEntryStream<K, V>
}
```

`K` may be a **class type or a primitive** — no boxing either way. The
bucket index uses `key.hash()` and bucket equality uses `==`; primitive
keys lower `.hash()` to the `__cajeta_hash_*` runtime helpers via a
compiler intrinsic. (Views are not allowed as `K` or `V` — see
`Views.md`.)

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

// Iterate via a stream view (keys / values / entries):
int64 distinct = counts.keys().count();
```

Pinned by `test/collections/HashMapTests.cpp`,
`test/collections/PrimitiveHashMapTests.cpp`, and the stream-view
suites `test/collections/HashMapStreamTests.cpp` /
`HashMapStreamParallelTests.cpp`.

### Open items

- Multiple-inherit `Stream<Pair<K, V>>` so the map IS its entry-
  stream (today `entries()` is the factory).
- `getOrDefault(K, V)`, `putIfAbsent(K, V)`, `merge`.

## `HashSet<T>` — shipped

Thin wrapper over a backing `HashMap<T, _>`. Same key constraint as
`HashMap` (class or primitive `T`).

```cajeta
public class HashSet<T> {
    public HashSet(int64 initialCapacity);     // power-of-2 capacity
    public void add(T v);
    public boolean contains(T v);
    public boolean remove(T v);                // true if present and removed
    public int64 count();
}
```

(No no-arg constructor and no `Stream` surface yet; `add` returns void.)

## `LinkedList<T>` — shipped

Doubly-linked list with head/tail pointers.

```cajeta
public class LinkedList<T> {
    public LinkedList();
    public int64 count();
    public T head();                           // O(1)
    public T tail();                           // O(1)
    public void add(T v);                      // alias of addTail
    public void addFirst(T v);                 // alias of addHead
    public void addTail(T v);                  // append at tail
    public void addHead(T v);                  // prepend at head
    public T popHead();                        // remove + return front
    public T popTail();                        // remove + return back
    public T get(int64 idx);                   // walk to index
    public boolean contains(T v);
    public boolean remove(T v);
}
```

`head()`/`tail()`/`get()`/`popHead()`/`popTail()` return a zero/null
sentinel on an empty list (no bounds error yet) — guard with `count()`.
No `Stream` surface yet.

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

## `Heap<T>` — shipped

Binary min-heap, ordered by `<` on `T` (the smallest element pops
first). No comparator parameter today.

```cajeta
public class Heap<T> {
    public Heap();
    public void push(T v);
    public T pop();                            // minimum
    public T peek();                           // minimum, no removal
    public boolean isEmpty();
    public int64 count();
}
```

No `Stream` surface yet.

## Ordered maps: `RedBlackTree<K, V>` / `BPlusTree<K, V>` — shipped

These replace the never-built `TreeMap` / `TreeSet`. Both are ordered
maps keyed by `<` / `>` on `K`; `RedBlackTree` is a balanced BST,
`BPlusTree` a cache-friendly B+ tree. Insert/lookup ship; **delete is
deferred**.

```cajeta
public class RedBlackTree<K, V> {       // BPlusTree<K, V> mirrors this
    public boolean isEmpty();
    public void put(K key, V value);
    public V get(K key);                       // 0/null on miss
    public boolean containsKey(K key);
    public K min();
    public K max();                            // RedBlackTree only
    public #ArrayList<K> keysInOrder();        // sorted keys
    public int64 count();
}
```

## `Collector<T, R>` and `Collectors` — shipped

`Collector<T, R>` reduces a `Stream<T>` into a single `R`. It is a
**class** holding three function-typed fields (not an interface with
methods); the `Stream<T>.collect<R>` terminal consumes it. All three
callables are required so the same collector works sequentially and
under `.parallel()` (each worker calls `supplier()` for a fresh partial;
the orchestrator merges with `combiner`).

```cajeta
public class Collector<T, R> {
    public () -> #R supplier;                  // fresh accumulator per worker
    public (R, T) -> #R accumulator;           // fold one element in
    public (R, R) -> #R combiner;              // merge two partials

    public Collector(() -> #R supplier,
                     (R, T) -> #R accumulator,
                     (R, R) -> #R combiner);
}

public class Collectors {
    public static #Collector<T, ArrayList<T>> toList<T>();
}
```

Only `toList<T>()` ships today (`toSet` / `toMap` / `counting` /
`joining` are not built). Used at the terminal end of a chain:

```cajeta
#ArrayList<int32> evens = xs.stream()
    .filter(isEven)
    .collect(Collectors.toList<int32>());
```

## For-loop desugaring through Stream

The `for (v : iterable)` form lowers to `iterable.next()` walking
calls when `iterable` is-a (multiple-inherits) `Stream<T>`. This is
the eventual story for HashMap / HashSet / LinkedList iteration —
the multiple-inheritance design eliminates the need for a separate
`Iterator<T>` abstraction.

Today the for-each loop works over arrays directly
(`for (int32 x : xs)`) via the array-iteration intrinsic; the
stream-receiver lowering is the next step.

## Planned: common collection interfaces

The container types ship today as standalone classes — there is no
common supertype. The plan is to **explore factoring out a thin
capability hierarchy** so generic code can operate over "any
collection" — common iteration, `count()`, `isEmpty()` — without
caring about the concrete type, while keeping the honest distinctions
between the kinds of container:

- **`Iterable<T>` — the real unifier:** `stream() -> Stream<T>` plus
  `count()` / `isEmpty()`. Everything that yields elements implements
  it: `ArrayList`, `HashSet`, `LinkedList`, `Heap`, the immutables,
  and (over keys / values / entries) the maps. This subsumes the
  "every container multiple-inherits `Stream<E>`" idea (see
  "For-loop desugaring through Stream") into a named, minimal contract.
- **`Map<K, V>` is its own interface, NOT a collection.** A map's
  element type is really `Pair<K, V>`, and `get(K)` / `containsKey(K)`
  have no analog on a list or set. Folding maps into one
  `Collection<T>` is the mistake Java explicitly avoids — keep them
  separate (a map is `Iterable<Pair<K, V>>` via `entries()`).
- **Mutation behind its own interface.** `add` / `remove` live on a
  `MutableCollection<T>` (with a map equivalent), so the immutable
  collections and read-only views simply don't implement it rather
  than carrying unsupported mutators.

Value: enables generic "operate on any sequence / any map" code and a
uniform iteration + `count()` story across the package.

Cost / sequencing: retrofitting interfaces onto the already-shipped
classes is a cross-cutting change with vtable / dispatch implications,
and it brushes against the operator-overloading-through-templates
limitation `Math` notes (ordered containers compare via `<` / `>`). So
the plan is to land the concrete types first, then introduce
`Iterable<T>` / `Map<K, V>` / `MutableCollection<T>` as a deliberate
follow-up pass in which every collection declares conformance at once.

## Larger-than-memory (`cajeta.collection.ltm`)

The in-memory collections above stay the defaults in
`cajeta.collection`. Disk-backed, larger-than-memory variants live in
a separate `cajeta.collection.ltm` package rather than as a mode flag
on the in-memory types — because they carry a genuinely different
contract: operations do I/O that can block or fail (`IoException`),
keys/values must be serializable, and there is an explicit lifecycle
(`open` / `close` / `flush` / `sync` / durability). Surfacing that at
the type level (you reach for `ltm.BPlusTree`) keeps the in-memory
common case free of disk-shaped API.

- **`ltm.BPlusTree<K, V>`** (planned, first): a paged B+ tree over
  `cajeta.io.file.File`. Nodes are fixed-size pages addressed by
  page-id × pageSize offsets; child links are page-ids, not object
  pointers; a buffer pool reads pages on demand and **evicts/frees**
  clean pages under memory pressure, flushing dirty ones. Same B+
  shape as the in-memory tree (linked leaves → range scans), so the
  algorithm is shared and only the node-access layer differs.
- **`ltm.HashMap<K, V>`** (later): extendible (or linear) hashing over
  the same pager — O(1) point lookups, no ordering. A directory keyed
  on the top bits of `key.hash()` points at bucket pages; a bucket
  overflow splits and doubles the directory only when local depth
  exceeds global depth.

Open design decision for the `ltm` types: how generic `K` / `V`
serialize into fixed-size page slots — an `Encoder<K>` / `Encoder<V>`
bound (`cajeta.wire`), versus restricting to fixed-width primitive
keys + `int8[]` values. Leaning toward the `Encoder` bound so the
types stay generic.

## Open items

Tracked in specs/Features.md:

- `Deque<T>` / `Stack<T>` (still designed-only); delete on the ordered
  trees; the `ltm` disk-backed variants.
- Multiple-inheritance from `Stream<E>` on `ArrayList`, `HashMap`,
  `HashSet`, `LinkedList`, `Heap` so they ARE streams (today they
  expose `stream()` / `keys()` / `values()` / `entries()` factories).
- More `Collectors` factories beyond `toList<T>()` (`toSet`, `toMap`,
  `counting`, `joining`).
- For-loop desugaring for `Stream`-typed receivers.

(Shipped since earlier drafts: `Collector<T, R>` + `Collectors.toList`
+ `Stream.collect<R>`, and fluent chained-form parsing —
`xs.stream().filter(p).count()` no longer needs intermediate locals.)
