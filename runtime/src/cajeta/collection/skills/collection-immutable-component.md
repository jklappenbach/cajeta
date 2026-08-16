---
id: collection-immutable-component
applies-to: [cajeta/collection/ImmutableList, cajeta/collection/ImmutableMap, cajeta/collection/ImmutableSet]
title: Immutable (frozen-snapshot) collections — List, Map, Set
description: Building and querying cajeta's read-only frozen-snapshot collections (ImmutableList, ImmutableMap, ImmutableSet) and how they consume a mutable source.
---

# Immutable collections: frozen snapshots

Three read-only collections, each a **frozen snapshot** built once from a
mutable source by moving its elements into a right-sized owned array. The
source is **consumed** at construction: you hand it over with `#` and it is dead
afterwards. The snapshot is
never mutated — so it is safe to share without defensive copies. Modeled on
Guava's `RegularImmutable*`. There are **no mutators**: to "change" one you build
a new snapshot from a fresh mutable source.

| You need… | Use | Built from |
|---|---|---|
| ordered, indexable, possibly-duplicate elements | **`ImmutableList<T>`** | `ArrayList<T>` |
| unique members, O(1) `contains` | **`ImmutableSet<T>`** | `ArrayList<T>` (de-duplicated, first-seen wins) |
| key→value, O(1) `get` | **`ImmutableMap<K, V>`** | `ArrayList<Pair<K, V>>` (duplicate keys: last-wins) |
| to mutate after building | not here — the mutable `ArrayList` / `HashMap` / `HashSet` |
| sorted-key iteration | not here — `cajeta.collection.BPlusTree` / `RedBlackTree` |

## Members and roles

- **`ImmutableList<T>`** — dense insertion-order array, O(1) `get`, O(n)
  `contains` / `indexOf`. No hash table.
- **`ImmutableSet<T>`** — dense first-seen-order `elements` array **plus** an
  open-addressing hash table for O(1) `contains`. Duplicates from the source are
  dropped (so the backing array may have unused trailing slack).
- **`ImmutableMap<K, V>`** — dense parallel `keyArr` / `valArr` (insertion
  order) **plus** a hash table for O(1) `get` / `containsKey`.

Each is independent — they do not share a base type or cooperate at runtime;
they cooperate only with their mutable *source* type at construction.

## Construction — consume a mutable source

The constructor is the whole story: it iterates the source in order, moves each
element into fresh `heap` arrays, and installs them. The source list is
**consumed** — the parameter is `#ArrayList<T>`, so you must write
`heap ImmutableList<T>(#src)` at the call site (a plain `src` is
`CAJETA_ERROR_TRANSFER_REQUIRED`), and `src` is dead once the constructor
returns: its elements have moved into the snapshot.

```cajeta
import cajeta.collection.ArrayList;
import cajeta.collection.ImmutableList;
import cajeta.lang.stream.ArrayStream;

ArrayList<int32> src = heap ArrayList<int32>();
src.add(10);
src.add(20);
src.add(30);

ImmutableList<int32> frozen = heap ImmutableList<int32>(#src);
// `src` is dead here — its elements moved into `frozen`, which never changes.

frozen.count();      // 3   (int64)
frozen.get(1);       // 20
frozen.get(99);      // 0   — zero value, no throw (see below)
frozen.indexOf(30);  // 2
frozen.contains(40); // false

ArrayStream<int32> s #= frozen.stream();  // owned, see "Ownership"
```

`ImmutableSet<T>` takes the same `ArrayList<T>`, dropping `==`-equal duplicates
(first occurrence wins); `ImmutableMap<K, V>` takes an
`ArrayList<Pair<K, V>>`, resolving duplicate keys last-wins:

```cajeta
import cajeta.collection.ArrayList;
import cajeta.collection.ImmutableMap;
import cajeta.lang.Pair;

ArrayList<Pair<string, int32>> entries = heap ArrayList<Pair<string, int32>>();
// ... populate with Pair entries ...
ImmutableMap<string, int32> m = heap ImmutableMap<string, int32>(#entries);

int32 one = m["one"];               // operator[] -> get("one")
boolean has = m.containsKey("two");
int64 n = m.count();                // int64
string firstKey = m.keyAt(0);       // dense, insertion order
int32 firstVal = m.valAt(0);
```

## Hashing and equality (Set, Map)

Same contract as `HashMap` / `HashSet`: `K.hash()` (or `T.hash()`) picks the
bucket, `==` decides equality. **Class keys/elements default to identity
semantics** — override `hash()` + `==` for value semantics. Primitive types
lower `.hash()` to the intrinsic. `ImmutableList` does not hash; its `contains` /
`indexOf` are linear `==` scans.

## Miss / out-of-range semantics — zero value, never throws

Every read returns the **type's zero value** (null for class `T`, 0 for
primitive `T`) instead of throwing:

- `ImmutableList.get(i)` / `ImmutableSet.get(i)` / `ImmutableMap.keyAt(i)` /
  `valAt(i)` on an out-of-range index → zero value.
- `ImmutableMap.get(key)` / `m[key]` on an absent key → zero value.

So a zero/null result is **ambiguous** between "absent" and "present-with-zero".
Use `containsKey` (Map) or `contains` (Set) to disambiguate. `indexOf` is the
exception: it returns `-1` (not a zero value) for "not found".

## Ownership & lifecycle across the boundary

- **Constructor `src` is consumed** — the formal is `#ArrayList<T>`, so call it as
  `(#src)`. The elements *move* into the snapshot's arrays; the source is dead at
  the call, not merely read.
- **`stream()` returns `#ArrayStream<T>` — ownership transfers to you** (List
  and Set only). The `#` marks the move; you own and dispose the returned
  stream. It is a view over the snapshot's frozen backing array, so keep the
  snapshot alive while streaming.
- `count()` returns `int64` (widened) on all three for parity with the
  hash-based collections; internal sizes are `int32`.

## What these do NOT do

- **No mutators at all** — no `add` / `put` / `remove` / `set` / `clear`. Build
  a new snapshot from a fresh source instead.
- **No bounds-checked accessor** — out-of-range is the zero-value sentinel
  above, not an exception (a checked variant lands with the error model).
- **`ImmutableMap` has no `stream()`** — walk entries by index with
  `keyAt(i)` / `valAt(i)` over `[0, count())`.
- **No `copyOf(HashMap)` / `copyOf(HashSet)`** convenience yet — you must
  materialize an `ArrayList` (`ArrayList<Pair<K, V>>` for the map) as the source.
- No removal, tombstones, or resize in the Set/Map hash tables — the immutable
  contract removes the need.

See `cajeta.collection.ArrayList`, `cajeta.collection.HashMap`,
`cajeta.collection.HashSet` for the mutable counterparts, `cajeta.lang.Pair` for
the map's entry type, and `cajeta.lang.stream.ArrayStream` for the stream view.
