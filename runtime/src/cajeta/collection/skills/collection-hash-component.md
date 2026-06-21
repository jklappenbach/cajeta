---
id: collection-hash-component
applies-to: [cajeta/collection/HashMap, cajeta/collection/HashSet]
title: HashMap & HashSet — open-addressing hash family
description: How to use cajeta's hash map/set pair — construction, key/value contracts, ownership, and the Stream views.
---

# HashMap & HashSet

The hash family in `cajeta.collection`. Reach for `HashMap<K, V>` for key→value
lookup, `HashSet<T>` for unique-membership. `HashSet<T>` is a **thin wrapper over
`HashMap<T, int8>`** (the value side is a 1-byte presence sentinel) — it adds no
storage logic of its own; everything below about probing, capacity, hashing, and
equality is inherited from `HashMap`.

## Members and roles
- **`HashMap<K, V>`** — the workhorse. Owns three parallel heap arrays (`keys`,
  `vals`, `state`) at one length; open addressing + linear probing, no bucket
  chains. This is the access point.
- **`HashSet<T>`** — delegates every call to a private `HashMap<T, int8>`:
  `add`→`put(v,1)`, `contains`→`containsKey`, `remove`→`remove`, `count`→`count`.
  No `keys()/values()/entries()` on the set surface — only `HashMap` exposes those.

`HashSet` exists partly to sidestep a HashMap-with-class-V miss-path quirk: it
always uses a primitive (`int8`) value side, so it never hits `get`'s null/0
default-of-V path.

## Construction & ownership
Both are heap objects taking a **power-of-2 initial capacity** the caller picks.
v1 does **not** validate power-of-2 — a non-power-of-2 breaks the
`hash & (cap-1)` mask indexing. There is no default/no-arg constructor.

```cajeta
import cajeta.collection.HashMap;
import cajeta.collection.HashSet;

#HashMap<int32, int32> counts = heap HashMap<int32, int32>(16);
HashSet<int32> seen = heap HashSet<int32>(16);
```

`put`/`add` do **not** take ownership of keys or values via `#` — they are stored
by the same value/reference convention as any field assignment; class-typed keys
held in the table are the caller's same instances.

## Key/value contract (the #1 correctness trap)
A `K` (and a `HashSet` `T`) must answer two things:
- **`key.hash()`** → bucket index.
- **`==`** → equality within a bucket.

For **class types** both default to *identity*: `hash()` is the inherited
`cajeta.lang.Object.hash()` (per-instance) and `==` is pointer identity. So two
distinct instances with equal fields are **different keys**. For value-keyed
semantics, override **both** `hash()` and `==` on the class (or apply `@AutoHash`)
— overriding only one silently corrupts lookups.

**Primitive K/T work without boxing** (int8/16/32/64, uint*, float, double,
boolean, char): `.hash()` lowers to a `__cajeta_hash_X` runtime intrinsic. So
`HashMap<int32, V>`, `HashMap<Tag, int32>`, and `HashSet<int32>` are all valid.

**Views are NOT allowed as K or V** — instantiating e.g. `HashMap<MyView, X>`
raises `CAJETA_ERROR_VIEW_AS_CLASS_FIELD` at compile time. Store the underlying
`byte[]` and build the view per access instead.

## Lookup return semantics — null/0 on miss
`get(key)` / `m[key]` return the value, or the **zero value of `V`** on miss
(null for class `V`, 0 for primitive `V`). They do **not** throw and there is no
`Optional`-returning get. To tell "absent" from "present-but-null/0", call
`containsKey(key)` first. `remove` returns `boolean` (was-present); it leaves a
reusable tombstone, it does not shrink the arrays.

## Capacity & lifecycle
Auto-grows when load factor (counting tombstones) exceeds 0.75: capacity doubles
and every live entry is reinserted, clearing tombstones. The table is a plain heap
object — it drops with normal scope/ownership rules; there is **no** `close()` and
no manual free. There is **no** `clear()`, no `putIfAbsent`, no bulk
`putAll`/constructor-from-collection, and (v1) **no `Iterator`** — traversal is
only via the Stream views below.

## Stream views (HashMap only) — ownership across the boundary
`keys()`, `values()`, and `entries()` each return an **owned** `#Stream<...>`
(transfer to the caller — the caller drives and drops it). They are **snapshots**
in slot-walk order (hash order, treat as unordered); mutating the map after
obtaining a stream leaves the stream's behavior undefined (Java-style fail-fast
contract, not enforced). `entries()` yields a **fresh heap `Pair<K,V>` per live
slot**.

```cajeta
import cajeta.collection.HashMap;
import cajeta.lang.Pair;
import cajeta.lang.stream.Stream;

#HashMap<int32, int32> m = heap HashMap<int32, int32>(16);
m.put(1, 100);
m.put(2, 200);

// Terminal: count / forEach drain the stream by pulling next() to exhaustion.
int32 n = m.keys().count();                 // 2
m.values().forEach((int32 v) -> total = total + v);

// Manual pull: next() returns Optional<Pair<K,V>>, present until exhausted.
#Stream<Pair<int32, int32>> es = m.entries();
Optional<Pair<int32, int32>> e = es.next();
while (e.isPresent()) {
    Pair<int32, int32> p = e.get();
    process(p.first(), p.second());
    e = es.next();
}
```

See the `cajeta.lang.stream` skills for the full `Stream`/`Optional` surface and
`cajeta.lang.Pair` for `first()`/`second()`.

## When to use which
- Key→value mapping, or you need to iterate keys/values/entries → **`HashMap`**.
- Unique-membership / set-presence only, no associated value → **`HashSet`** (less
  surface, primitive-safe value side).
- Need sorted/ordered keys or range queries → **not here**; use
  `cajeta.collection.RedBlackTree` / `BPlusTree`.
- Need an immutable snapshot → `ImmutableMap` / `ImmutableSet`.
