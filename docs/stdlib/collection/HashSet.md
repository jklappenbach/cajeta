# HashSet\<T\>

`cajeta.collection.HashSet` — hash-based set of unique values, a thin wrapper
around `HashMap<T, T>` storage: `add` writes, `contains` reads, `remove`
deletes. It inherits HashMap's semantics wholesale — `T.hash()` picks the
bucket, `==` decides membership within it, and both class `T` (identity
semantics unless `hash()` / `operator==` are overridden) and primitive `T`
work without boxing.

```cajeta
HashSet<int32> seen = heap HashSet<int32>(16);
seen.add(7);
seen.add(7);                    // already a member — no-op
boolean has = seen.contains(7); // true
seen.remove(7);
int64 n = seen.count();         // 0
```

## Methods

| Signature | |
|---|---|
| `HashSet(int64 initialCapacity)` ⚑ | Construct with the given initial bucket capacity (must be a power of 2) |
| `void add(T value)` | Insert `value` into the set |
| `boolean contains(T value)` | True iff `value` was previously added and not since removed |
| `boolean remove(T value)` | Remove `value`; returns true iff it was present |
| `int64 count()` | Number of unique members currently in the set |

⚑ = `@EntryPoint`

## See also

- Tour: [HashSetDemo](../../../samples/tour/src/main/cajeta/tour/collection/HashSetDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/HashSet.cajeta`](../../../runtime/src/cajeta/collection/HashSet.cajeta)
- [HashMap](HashMap.md) — the backing storage
- [ImmutableSet](ImmutableSet.md) — the frozen counterpart
