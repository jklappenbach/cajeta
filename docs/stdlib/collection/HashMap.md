# HashMap\<K, V\>

`cajeta.collection.HashMap` — hash-based map from keys to values, backed by a
SwissTable (SSE2-style open addressing with SIMD metadata probing) over a
dense entry array. `key.hash()` indexes the table and `==` decides equality
within a probe: class `K` defaults to identity semantics (override `hash()`
and `==`, or apply `@AutoHash`, for value keys), while primitive `K` lowers
`.hash()` to a runtime helper — no boxing. Capacity is a power of two `>= 16`;
the table doubles when non-empty slots exceed 0.75 of capacity, and `remove`
leaves a tombstone that `put` reuses and `get` probes past.

```cajeta
HashMap<int32, int32> counts = heap HashMap<int32, int32>(16);
counts.put(1, 100);
counts[2] = 200;                 // subscript sugar over put
int32 v = counts[2];             // ... and over get
if (counts.containsKey(1)) {
    counts.remove(1);
}
int64 n = counts.count();        // 1
```

## Methods

| Signature | |
|---|---|
| `HashMap(int64 initialCapacity)` ⚑ | Construct a HashMap (capacity is rounded up to a power of two) |
| `void put(K key, V value)` | Insert or replace the value at `key` |
| `V get(K key)` | Retrieve the value at `key`, or null/zero if absent |
| `boolean containsKey(K key)` | Test whether `key` is present |
| `boolean remove(K key)` | Remove the entry at `key`; returns true iff it was present |
| `int64 count()` | Live entry count |
| `V operator[] (K key)` | Subscript sugar over `get` |
| `void operator[]= (K key, V value)` | Subscript-assignment sugar over `put` |
| `#Stream<K> keys()` | Stream over the live keys, slot-walk order |
| `#Stream<V> values()` | Stream over the live values, slot-walk order |
| `#Stream<Pair<K, V>> entries()` | Stream of (key, value) pairs, one fresh Pair per live slot |

⚑ = `@EntryPoint`

## See also

- Tour: [HashMapDemo](../../../samples/tour/src/main/cajeta/tour/collection/HashMapDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/HashMap.cajeta`](../../../runtime/src/cajeta/collection/HashMap.cajeta)
- [HashSet](HashSet.md) — the set built on this map
- [ImmutableMap](ImmutableMap.md) — the frozen counterpart
- [Pair](../lang/Pair.md) — `entries()`'s element type
