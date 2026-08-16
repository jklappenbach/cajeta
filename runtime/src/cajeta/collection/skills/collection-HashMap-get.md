---
id: collection-HashMap-get
applies-to: [cajeta/collection/HashMap.get]
title: HashMap.get — raw value + null/0 sentinel on miss (no Optional)
description: get(K) returns the stored V directly, with the zero value of V (null for class V, 0 for primitive V) on a miss — null-check it, do not treat it as an Optional.
---

# HashMap.get

```cajeta
public V get(K key)          // m.get(key)
public V operator[] (K key)  // m[key] — delegates straight to get
```

`get` returns the **raw `V`** bound to `key`, or the **zero value of `V`** on a
miss: `null` when `V` is a class type, `0`/`false`/`\0` when `V` is a primitive.
It does **not** return `Optional<V>` and it does **not** throw. So the result of a
class-`V` `get` is a possibly-null `V` pointer — treat it as one.

```cajeta
import cajeta.collection.HashMap;

HashMap<SessionId, Session> sessions = heap HashMap<SessionId, Session>(64);
sessions.put(id, session);

Session s = sessions.get(id);   // raw Session, or null on miss
if (s == null) {
    s = openSession(id);
}
```

## The trap: it is NOT an Optional

The single sharp edge. Because `V` for a class type comes back as a bare nullable
pointer, the recovery code is a `== null` / `!= null` check — **not**
`isPresent()` / `isEmpty()` / `.get()`. Calling `Optional` methods on the result
either won't compile (the value isn't an `Optional`) or, if `V` itself happens to
be `Optional<…>`, dispatches on a possibly-null pointer. `cajeta.collection.Cache`
documents this at three call sites; it wraps `get`'s raw node in its own
`Optional<V>` only after a manual null-check:

```cajeta
CacheNode<K, V> node = this.entries.get(key);   // entries is a HashMap
if (node == null) {
    return stack Optional<V>(false, #null);      // miss → empty
}
// ... use node ...
```

## Absent vs. present-but-null/0 — disambiguate with containsKey

`get` collapses two cases into one sentinel: a key that was never inserted, and a
key whose stored value really is `null`/`0`. When the difference matters, call
`containsKey(key)` first (a second probe), then `get`. If you never store
`null`/`0` values, the sentinel is unambiguous and the extra probe is unnecessary.

## Ownership

The returned class-`V` is **borrowed** — it is the same instance the map still
holds in its `vals` array, not a copy and not a transfer. Do not `#`-move it or
free it; its lifetime is the map's. A later `put` to the same key, `remove`, or a
load-factor `resize` may overwrite or relocate the entry, so don't retain the
reference across mutations if you need it to stay valid. Primitive `V` is returned
by value.

## Side effects, ordering, cost

- **No side effects** — `get` is read-only: it does not touch access time, move
  entries, evict, or create tombstones (contrast `Cache.get`, which touches LRU
  state and may evict). Safe to call in any order after construction.
- **Probing** — O(1) average. Walks linear-probe slots from `key.hash() &
  (cap-1)`, transparently skipping `TOMBSTONE` slots, stopping at the first
  `EMPTY`. A table thrashed with `remove`+`put` degrades probe length until the
  next `resize` compacts it.
- A miss allocates a throwaway 1-element `V[]` to source the correctly-typed zero
  value; it drops at scope exit and never escapes. Don't depend on this internal —
  just read the returned value.

## Found-the-wrong-method?

`get` does the lookup only. It does not insert-on-miss (no `getOrDefault` /
`putIfAbsent` / `computeIfAbsent` in v1), does not remove, and does not iterate.
For key/value contract details (`hash()` + `==`, primitive vs class keys, views
banned as `K`/`V`) and the Stream views, see the component skill
`collection-hash-component` (`cajeta/collection/HashMap`).
