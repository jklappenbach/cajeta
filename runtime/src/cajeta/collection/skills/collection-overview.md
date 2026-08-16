---
id: collection-overview
applies-to: [cajeta.collection]
title: cajeta.collection — orientation and task→type routing
description: Pick the right collection (list / hash / ordered-tree / immutable / cache / disk) and learn the library-wide K-type, null-on-miss, and #-ownership rules.
---

# cajeta.collection — what to reach for

General-purpose in-memory (and one disk-backed) container library: sequences, hash
maps/sets, ordered maps, frozen snapshots, an LRU/TTL cache, a priority queue, and the
`Stream.collect` plumbing. Almost everything is `heap`-allocated and generic over its
element/key/value type. Read this page first to choose a type and to learn the rules that
hold across *all* of them; then open the specific class's source for method detail.

## Task → type routing

| You want… | Use | Notes |
|---|---|---|
| Growable indexed sequence | `ArrayList<T>` | doubling array; `add/get/set/sort`; the default `Collectors.toList` accumulator |
| Stack / queue / deque (push-pop at ends) | `LinkedList<T>` | `addHead/addTail/popHead/popTail`, O(1) ends |
| Priority queue / min-heap | `Heap<T>` | `push/peek/pop`; smallest by `<` (max-heap: reverse `<`) |
| Key→value lookup, unordered | `HashMap<K,V>` | open addressing; `put/get/containsKey/remove`, `[]` sugar |
| Membership set, unordered | `HashSet<T>` | thin wrapper over `HashMap<T,int8>` |
| Key→value **in sorted order** (min/max/in-order) | `RedBlackTree<K,V>` | BST, O(log n); **no delete yet** |
| Sorted map with high fan-out / range scans | `BPlusTree<K,V>` | shallow tree, linked leaves; **no delete yet** |
| Sorted map **bigger than RAM**, on disk | `cajeta.collection.ltm.LtmBPlusTree<K,V>` | needs `Encoder`s; must `close()`/`flush()` |
| Frozen, shareable snapshot | `ImmutableList/Set/Map<…>` | built once from an `ArrayList`; no mutators |
| Memoize-with-eviction (LRU + optional TTL) | `Cache<K,V>` | **returns `Optional<V>`, the exception to the null-on-miss rule** |
| Sort / binary-search a raw `T[]` | `Sort` (static) | comparator seam `(T,T)->int32`; `sort` unstable, `sortStable` stable |
| Drain a `Stream` into a collection | `Collectors.toList<T>()` + `Stream.collect` | see `Collector` for custom reductions |

Negative rows — capabilities **not** here, so don't hunt for them:
- **No `Iterator<T>`.** Walk a `HashMap` via the snapshot streams `keys()/values()/entries()`; walk ordered trees via `keysInOrder()`.
- **No bounds checks / no thrown exceptions yet.** Out-of-range and empty access return a zero value (see below), they do not throw. Exception-throwing variants land with the error model.
- **No delete on `RedBlackTree` / `BPlusTree` / `LtmBPlusTree`** — insert/lookup only in v1.
- **No `insert`/`remove`-at-index on `ArrayList`**; no `Optional`-returning list peeks.
- **Views may not be a `K` or `V`** — `HashMap<MyView,…>` etc. is a compile error (`CAJETA_ERROR_VIEW_AS_CLASS_FIELD`); store the underlying `byte[]` and rebuild the view per access.

## Library-wide invariants — learn once, apply everywhere

- **K/T type requirements depend on the structure:**
  - Hash-based (`HashMap`, `HashSet`, `ImmutableMap`, `ImmutableSet`) need **`K.hash()` + `==`**. Both primitive K (lowered to a `__cajeta_hash_X` intrinsic) and class K work, no boxing.
  - Ordered (`RedBlackTree`, `BPlusTree`, `LtmBPlusTree`, `Heap`, `Sort` natural-order) need **`<` / `>`** on K. Primitive K works today; **class K needs `operator<`/`operator>` overloads, which don't link yet** (the v1 operator-overloading-through-templates limitation).
  - `LinkedList`/`ImmutableList` need only `==`; `ArrayList` needs nothing of T.
- **Class keys default to *identity*.** Class `hash()`/`==` are pointer identity, so two field-equal instances are different keys. Override **both** `hash()` and `==` for value-keyed semantics (planned `@AutoHash` will synthesize them); overriding only one breaks the bucket contract.
- **Null-on-miss, NOT `Optional`.** Class-typed `get`/`min`/`max`/`peek`/`pop`/`head`/`tail`/`keyAt` return the **raw value** — `null` for a class T, `0` for a primitive T — when absent or empty. They never throw and never wrap in `Optional`. Disambiguate "absent" from "present-but-null/zero" with `containsKey`/`contains`/`count`. **Exception:** `Cache.get` returns a real `Optional<V>`.
- **`#` transfer on returns that hand you ownership.** Stream/owned-collection returns are marked `#` and the caller owns the result: `stream()`, `HashMap.keys()/values()/entries()`, `RedBlackTree/BPlusTree.keysInOrder()` (`#ArrayList`), and `Collectors.toList()` (`#Collector`). When you store a freshly-built array/collection into a field, transfer with `#` (e.g. `this.data = #grown`) or the local's scope-exit drop frees the storage you just installed.
- **Construction:** all are `heap`/`stack`-constructed; there are no global factories. `HashMap`/`HashSet`/`ImmutableMap`/`ImmutableSet`/`Cache` take an initial capacity or a source list. **`HashMap` has no no-arg constructor and its `initialCapacity` must be a power of 2** (mask indexing; v1 doesn't validate). Tables auto-grow at 0.75 load.
- **Snapshot streams.** `HashMap` stream views are point-in-time snapshots; mutating the map after taking one leaves the stream's behavior undefined (same rule as Java's non-snapshot iterators).
- **Not thread-safe.** `HashMap`, `Cache`, etc. assume single-threaded access; wrap in `Mutex.withLock` to share across threads.

## Canonical end-to-end example

```cajeta
import cajeta.collection.HashMap;
import cajeta.collection.ArrayList;
import cajeta.collection.Collectors;
import cajeta.lang.stream.ArrayStream;

// Hash map: power-of-2 capacity, null/0 on miss (guard with containsKey).
HashMap<Tag, int32> counts = heap HashMap<Tag, int32>(16);
counts.put(tag, 1);
counts[tag] = counts[tag] + 1;          // subscript sugar over get/put
if (counts.containsKey(tag)) {
    int32 n = counts.get(tag);
}

// Stream → owned collection. Both results' ownership transfers (#).
int32[] data = { 3, 1, 2 };
ArrayList<int32> xs = (heap ArrayStream<int32>(data, 3))
    .collect(Collectors.toList<int32>());
xs.sort();                              // [1, 2, 3], in place
```

## Disambiguation

- **`HashMap` vs `RedBlackTree` vs `BPlusTree`** — hash for plain O(1) lookup with no
  ordering; red-black when you need sorted iteration / min / max of a binary tree;
  B+ tree when fan-out and leaf-linked range scans matter (and it's the in-memory twin of
  the disk `LtmBPlusTree`).
- **`ArrayList` vs `LinkedList`** — array for indexed/random access and cache locality;
  linked for O(1) end insertion/removal (deque/stack).
- **`ImmutableList/Set/Map` vs the mutable kinds** — freeze when you'll share a read-only
  snapshot without defensive copies; to "change" one, build a new one from an `ArrayList`.
- **`Cache` vs `HashMap`** — `Cache` when you want bounded size with LRU eviction and/or
  TTL; plain `HashMap` when entries should live until you remove them.

## Setup / pointers

Import per type, e.g. `import cajeta.collection.HashMap;` (`LtmBPlusTree` lives in the
nested `cajeta.collection.ltm` package). Ordering-dependent types (`RedBlackTree`,
`BPlusTree`, `Heap`, `Sort` natural order) are primitive-K-only until class operator
overloading lands. For per-class construction, method signatures, and ownership detail,
read the class source under `runtime/src/cajeta/collection/`; `Sort`/`Collector` carry the
comparator and stream-collect contracts respectively.
