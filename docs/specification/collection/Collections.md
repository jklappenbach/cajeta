# `cajeta.collection` — Lists, Sets, Maps, and friends

This package holds the container types in `runtime/src/cajeta/collection/`.
Today each container exposes its elements as a `cajeta.lang.stream.Stream<E>`
through a factory method, which is `stream()`, or `keys()`/`values()`/`entries()`
on `HashMap`. The multiple-inheritance design, in which a container is
itself a stream, is still pending. See Streams.md for the full stream
protocol and combinator surface.

All containers report their element count via `count()`, and never via
`size` or `length`. `count()` returns `int64` except on `ArrayList`, where
it is `int32`.

## Collections borrow their elements by default — transfer is the caller's choice

Collections are not containers. They can be treated like containers by
transferring ownership in, but they are not designed to own by default. Element
and key parameters on the collections below are declared plainly as `T` / `K` /
`V`, with one exception noted at `HashMap.operator[]=`. Passing `x` lends and
passing `#x` transfers. Both spellings are legal, and the choice is the
caller's:

```cajeta
Cell c = heap Cell(7);
xs.add(c);       // lends — the list holds a borrow; `c` still owns the instance
xs.add(#c);      // transfers — the list takes the title
int32 n = c.n;   // still fine: `#` moves the title, not the binding
```

Genuine containers do declare `#` formals. A genuine container is one whose
storage outlives the caller's frame, such as `JsonValue.setArray` / `setObject`,
where the DOM owns its children, or `Pair`. A `#` formal obliges the caller to
write `#` whenever the argument is a local the frame owns. A fresh value, which
is a literal or a `heap T(...)` / `stack T(...)` construction, passes plain,
because an unowned temporary has no title to surrender.

Three things to know before writing collection code:

- **Each entry records the mode it arrived in.** A `#`-tendered entry belongs to
  the collection, so teardown drops it, `remove` hands the title back, and a
  replace displaces exactly one value. A lent entry is never reclaimed, so its
  real owner must outlive the collection.
- **The source stays readable.** A transfer DEMOTES its source to a borrow of
  the same live instance, as `MemoryModel.md` describes. What you must not do is
  read it after the collection tears down. That read dangles, and nothing
  diagnoses it yet (§1.7).
- **String is an ordinary element.** `list.add(s)` lends a `String` and
  `list.add(#s)` transfers it, exactly as for any other class. A lent `String`
  must outlive the list. If you would rather the list hold something
  independent of the caller's lifetime, `s.substring(0, s.count())` returns a
  fresh `#String`.

Keys MAY be transferred, as in `map.put(#k, v)`, and when they are, hashing
matters. `String` and primitives are VALUE-hashed, so a fresh equal key finds a
surrendered entry. A user class is IDENTITY-hashed by default, so it does not.
Replacing a value under an owned class key therefore has no spelling today.
`map.update` is an open design question, and `OwnedKeyLookupTests` pins the
identity MISS so that the day structural equality lands for classes, it fails
loudly.

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

`ArrayList<T>` is a dynamic array, a heap-allocated `T[]` that grows on
demand. The source is at `runtime/src/cajeta/collection/ArrayList.cajeta`.

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
int32 sum #= list.stream().reduce(0,
    (int32 a, int32 b) -> { return a + b; });
```

It is pinned by `test/parser/ArrayListTests.cpp`, which holds 7 tests.

### Open items

- Multiple-inherit `Stream<T>` so `for (v : list)` works without the
  `.stream()` factory.
- `remove(int32 i)`, `clear()`, `contains(T v)`, `indexOf(T v)`.
- Bounds checks at `get`/`set`/`remove` once error model wiring is
  ready.

## `HashMap<K, V>` — shipped

`HashMap<K, V>` is an open-addressing hash map with linear probing and
tombstones. The source is at `runtime/src/cajeta/collection/HashMap.cajeta`.

```cajeta
public class HashMap<K, V> {
    public HashMap(int64 initialCapacity);     // power-of-2 capacity
    public void put(K key, V value);
    public V get(K key);                        // returns 0/null on miss
    public boolean containsKey(K key);
    public boolean remove(K key);              // leaves a reusable tombstone
    public int64 count();

    public V operator[](K key);                // sugar for get
    public void operator[]=(#K key, #V value); // put, forwarding the caller's mode

    // Splittable stream views (snapshots; slot-walk order):
    public #Stream<K> keys();                  // HashMapKeyStream<K, V>
    public #Stream<V> values();                // HashMapValueStream<K, V>
    public #Stream<Pair<K, V>> entries();      // HashMapEntryStream<K, V>
}
```

The `#K` / `#V` on `operator[]=` are forwards. They thread the caller's
transfer word through to `put`, since a plain forward would read all-borrow and
record every indexed store as borrowed no matter how the caller spelled it. They
are never an unconditional take. So `m[k] = v` stays mode-carrying, exactly like
`put`. A fresh value passes plain, so `counts["grapes"] = 7` and
`m["k"] = heap Box(5)` both compile. A named local the frame owns must be
surrendered explicitly, as `m[#k] = v`, which is the same `#` that `put(#k, v)`
would ask for.

`K` may be a class type or a primitive, with no boxing either way. The
bucket index uses `key.hash()` and bucket equality uses `==`. Primitive
keys lower `.hash()` to the `__cajeta_hash_*` runtime helpers via a
compiler intrinsic. Views are not allowed as `K` or `V`, as `Views.md`
explains.

Capacity is a power of 2. The map auto-grows past a 0.75 load factor,
with tombstone compaction on each resize.

### Examples

```cajeta
import cajeta.collection.HashMap;

HashMap<String, int32> counts = heap HashMap<String, int32>(64);
counts.put("apples", 3);
counts.put("oranges", 5);

int32 apples = counts.get("apples");          // 3
boolean has = counts.containsKey("kiwis");    // false

// Operator overloads — a fresh key/value needs no `#`
counts["grapes"] = 7;
int32 grapes = counts["grapes"];

// ...but a key the frame owns is surrendered explicitly
String kiwis = "ki" + "wis";
counts[#kiwis] = 2;
int32 kiwiCount = counts[kiwis];              // `kiwis` is demoted, still looks up

// Iterate via a stream view (keys / values / entries):
int64 distinct #= counts.keys().count();
```

It is pinned by `test/collections/HashMapTests.cpp`,
`test/collections/PrimitiveHashMapTests.cpp`, and the stream-view
suites `test/collections/HashMapStreamTests.cpp` /
`HashMapStreamParallelTests.cpp`.

### Open items

- Multiple-inherit `Stream<Pair<K, V>>` so the map IS its entry
  stream. Today `entries()` is the factory.
- `getOrDefault(K, V)`, `putIfAbsent(K, V)`, `merge`.

## `HashSet<T>` — shipped

`HashSet<T>` is a thin wrapper over a backing `HashMap<T, _>`. It has the
same key constraint as `HashMap`, so `T` is a class or a primitive.

```cajeta
public class HashSet<T> {
    public HashSet(int64 initialCapacity);     // power-of-2 capacity
    public void add(T v);
    public boolean contains(T v);
    public boolean remove(T v);                // true if present and removed
    public int64 count();
}
```

There is no no-arg constructor and no `Stream` surface yet, and `add` returns
void.

## `LinkedList<T>` — shipped

`LinkedList<T>` is a doubly-linked list with head and tail pointers.

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

`head()`/`tail()`/`get()`/`popHead()`/`popTail()` return a zero or null
sentinel on an empty list, because there is no bounds error yet, so guard
with `count()`. There is no `Stream` surface yet.

## `Deque<T>` / `Stack<T>` — designed

`Deque<T>` is a double-ended queue, and `Stack<T>` exposes the LIFO
subset of `push` / `pop` / `peek`. Both back onto a circular array
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

Both multiple-inherit `Stream<T>`. Deque streams in front-to-back order,
and Stack streams in pop order, top to bottom.

## `Heap<T>` — shipped

`Heap<T>` is a binary min-heap, ordered by `<` on `T`, so the smallest
element pops first. There is no comparator parameter today.

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

There is no `Stream` surface yet.

## Ordered maps: `RedBlackTree<K, V>` / `BPlusTree<K, V>` — shipped

These replace the never-built `TreeMap` / `TreeSet`. Both are ordered
maps keyed by `<` / `>` on `K`. `RedBlackTree` is a balanced BST, and
`BPlusTree` is a cache-friendly B+ tree. Insert and lookup ship, and
delete is deferred.

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
class holding three function-typed fields, and the `Stream<T>.collect<R>`
terminal consumes it. All three callables are required so the same
collector works sequentially and under `.parallel()`. Each worker calls
`supplier()` for a fresh partial, and the orchestrator merges with
`combiner`.

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

Only `toList<T>()` ships today, and `toSet` / `toMap` / `counting` /
`joining` are not built. It is used at the terminal end of a chain:

```cajeta
ArrayList<int32> evens #= xs.stream()
    .filter(isEven)
    .collect(Collectors.toList<int32>());
```

`collect<R>` returns an owned `#R`. The collected list is a fresh value
with no borrow into the stream, so it outlives the stream temporary that
is freed at the end of the statement. Bind it with `#=`. A plain `=` is
`CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER`.

## For-loop desugaring through Stream

The `for (v : iterable)` form lowers to `iterable.next()` walking
calls when `iterable` multiple-inherits `Stream<T>`. This is the
eventual story for HashMap / HashSet / LinkedList iteration, because
the multiple-inheritance design eliminates the need for a separate
`Iterator<T>` abstraction.

Today the for-each loop works over arrays directly, as
`for (int32 x : xs)`, via the array-iteration intrinsic. The
stream-receiver lowering is the next step.

## Planned: common collection interfaces

The container types ship today as standalone classes, and there is no
common supertype. The plan is to explore factoring out a thin
capability hierarchy so template code can operate over any collection,
with common iteration, `count()` and `isEmpty()`, without caring about
the concrete type, while keeping the honest distinctions between the
kinds of container:

- **`Iterable<T>` is the real unifier.** It is `stream() -> Stream<T>`
  plus `count()` / `isEmpty()`. Everything that yields elements
  implements it, meaning `ArrayList`, `HashSet`, `LinkedList`, `Heap`,
  the immutables, and the maps over keys, values and entries. This
  subsumes the idea that every container multiple-inherits `Stream<E>`
  (see "For-loop desugaring through Stream") into a named, minimal
  contract.
- **`Map<K, V>` is its own interface and never a collection.** A map's
  element type is really `Pair<K, V>`, and `get(K)` / `containsKey(K)`
  have no analog on a list or set. Folding maps into one
  `Collection<T>` is the mistake Java explicitly avoids, so they stay
  separate. A map is `Iterable<Pair<K, V>>` via `entries()`.
- **Mutation behind its own interface.** `add` / `remove` live on a
  `MutableCollection<T>`, with a map equivalent, so the immutable
  collections and read-only views simply do not implement it and never
  carry unsupported mutators.

The value is that it enables code that operates on any sequence or any
map, and a uniform iteration and `count()` story across the package.

The cost and sequencing are the reason to wait. Retrofitting interfaces
onto the already-shipped classes is a cross-cutting change with vtable
and dispatch implications, and it brushes against the
operator-overloading-through-templates limitation `Math` notes, since
ordered containers compare via `<` / `>`. So the plan is to land the
concrete types first, then introduce
`Iterable<T>` / `Map<K, V>` / `MutableCollection<T>` as a deliberate
follow-up pass in which every collection declares conformance at once.

## Larger-than-memory (`cajeta.collection.ltm`)

The in-memory collections above stay the defaults in
`cajeta.collection`. Disk-backed, larger-than-memory variants live in
a separate `cajeta.collection.ltm` package instead of behind a mode
flag on the in-memory types, because they carry a genuinely different
contract. Operations do I/O that can block or fail with `IoException`,
keys and values must be serializable, and there is an explicit
lifecycle of `open` / `close` / `flush` / `sync` and durability.
Surfacing that at the type level, so that you reach for
`ltm.BPlusTree`, keeps the in-memory common case free of disk-shaped
API.

- **`ltm.BPlusTree<K, V>`** (planned, first) is a paged B+ tree over
  `cajeta.io.file.File`. Nodes are fixed-size pages addressed by
  page-id × pageSize offsets, and child links are page-ids instead of
  object pointers. A buffer pool reads pages on demand and evicts and
  frees clean pages under memory pressure, flushing dirty ones. It has
  the same B+ shape as the in-memory tree, with linked leaves for range
  scans, so the algorithm is shared and only the node-access layer
  differs.
- **`ltm.HashMap<K, V>`** (later) is extendible or linear hashing over
  the same pager, with O(1) point lookups and no ordering. A directory
  keyed on the top bits of `key.hash()` points at bucket pages. A
  bucket overflow splits, and it doubles the directory only when local
  depth exceeds global depth.

The open design decision for the `ltm` types is how the template
parameters `K` / `V` serialize into fixed-size page slots. The choices
are an `Encoder<K>` / `Encoder<V>` bound from `cajeta.wire`, or a
restriction to fixed-width primitive keys and `int8[]` values. We lean
toward the `Encoder` bound so the types stay fully parameterized.

## Open items

These are tracked in specs/Features.md:

- `Deque<T>` / `Stack<T>`, which are still designed-only, delete on the
  ordered trees, and the `ltm` disk-backed variants.
- Multiple-inheritance from `Stream<E>` on `ArrayList`, `HashMap`,
  `HashSet`, `LinkedList`, `Heap` so they ARE streams. Today they
  expose `stream()` / `keys()` / `values()` / `entries()` factories.
- More `Collectors` factories beyond `toList<T>()` (`toSet`, `toMap`,
  `counting`, `joining`).
- For-loop desugaring for `Stream`-typed receivers.

Shipped since earlier drafts are `Collector<T, R>`, `Collectors.toList`,
`Stream.collect<R>`, and fluent chained-form parsing, so
`xs.stream().filter(p).count()` no longer needs intermediate locals.
