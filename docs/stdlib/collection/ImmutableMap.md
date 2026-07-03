# ImmutableMap\<K, V\>

`cajeta.collection.ImmutableMap` — immutable, hash-indexed map: a frozen
snapshot of `(key, value)` pairs indexed by a read-only SwissTable. Entries
are stored once in dense arrays (insertion order, cheap to index by
`keyAt` / `valAt`) with an O(1) `get` / `containsKey` index built once — no
put, remove, tombstones, or resize. Built from an `ArrayList<Pair<K, V>>`;
duplicate keys resolve last-wins. Hashing and equality follow the `HashMap`
contract: `K.hash()` indexes, `==` decides key equality.

```cajeta
ArrayList<Pair<int32, int32>> src = heap ArrayList<Pair<int32, int32>>();
src.add(heap Pair<int32, int32>(1, 100));
src.add(heap Pair<int32, int32>(2, 200));

ImmutableMap<int32, int32> m = heap ImmutableMap<int32, int32>(src);
int32 v = m.get(2);              // 200
boolean has = m.containsKey(3);  // false
int32 k0 = m.keyAt(0);           // 1 — dense, insertion order
```

## Methods

| Signature | |
|---|---|
| `ImmutableMap(ArrayList<Pair<K, V>> src)` ⚑ | Build a frozen map from `src` (last-wins on duplicate keys); the source is untouched |
| `V get(K key)` | Value bound to `key`, or the type's zero value if absent |
| `boolean containsKey(K key)` | True iff `key` is present |
| `V operator[] (K key)` | Subscript sugar over `get` |
| `int64 count()` | Number of entries |
| `boolean isEmpty()` | True iff the map holds no entries |
| `K keyAt(int32 i)` | Key at dense index `i` (0-based, insertion order); zero value if out of range |
| `V valAt(int32 i)` | Value at dense index `i` (0-based, insertion order); zero value if out of range |

⚑ = `@EntryPoint`

## See also

- Tour: [ImmutableMapDemo](../../../samples/tour/src/main/cajeta/tour/collection/ImmutableMapDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/ImmutableMap.cajeta`](../../../runtime/src/cajeta/collection/ImmutableMap.cajeta)
- [HashMap](HashMap.md) — the mutable counterpart
- [Pair](../lang/Pair.md) — the construction element type
- [ImmutableList](ImmutableList.md), [ImmutableSet](ImmutableSet.md) — the other frozen collections
