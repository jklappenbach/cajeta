# ImmutableSet\<T\>

`cajeta.collection.ImmutableSet` — immutable, hash-indexed set: a frozen,
de-duplicated snapshot of an `ArrayList<T>`. Members are stored once in a
dense array (so the set is cheaply indexable and streamable in first-seen
order) with a separate open-addressing table for O(1) `contains`, built once
at construction — no removal, tombstones, or resize. Hashing and equality
follow the `HashMap` / `HashSet` contract: `T.hash()` selects the bucket, `==`
decides membership.

```cajeta
ArrayList<int32> src = heap ArrayList<int32>();
src.add(1);
src.add(2);
src.add(2);

ImmutableSet<int32> set = heap ImmutableSet<int32>(src);
int64 n = set.count();          // 2 — duplicate dropped
boolean has = set.contains(2);  // true
int32 first = set.get(0);       // 1 — first-seen order preserved
```

## Methods

| Signature | |
|---|---|
| `ImmutableSet(ArrayList<T> src)` ⚑ | Build a frozen, de-duplicated set from `src` (first occurrence wins); the source is untouched |
| `int64 count()` | Number of unique members |
| `boolean isEmpty()` | `count() == 0` |
| `boolean contains(T v)` | True iff `v` is a member; O(1) average |
| `T get(int32 i)` | Member at dense index `i` (0-based, first-seen order), or the type's zero value if out of range |
| `#ArrayStream<T> stream()` | Heap-allocated `ArrayStream` over the unique members, first-seen order |

⚑ = `@EntryPoint`

## See also

- Tour: [ImmutableSetDemo](../../../samples/tour/src/main/cajeta/tour/collection/ImmutableSetDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/ImmutableSet.cajeta`](../../../runtime/src/cajeta/collection/ImmutableSet.cajeta)
- [HashSet](HashSet.md) — the mutable counterpart
- [ImmutableList](ImmutableList.md) — the frozen list
- [ArrayList](ArrayList.md) — the construction source
