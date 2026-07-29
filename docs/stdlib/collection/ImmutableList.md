# ImmutableList\<T\>

`cajeta.collection.ImmutableList` — immutable, array-backed list: a frozen
snapshot of an `ArrayList`. The backing array is copied once at construction
and never mutated, so reads are O(1) and the structure is safe to share
without defensive copies. There are no mutators; to "change" one, build a new
list from a fresh `ArrayList`. Like `ArrayList`, `get` returns the type's zero
value on an out-of-range index rather than throwing.

```cajeta
ArrayList<int32> src = heap ArrayList<int32>();
src.add(10);
src.add(20);
src.add(30);

// Freeze it — `src` stays owned by the caller and is untouched.
ImmutableList<int32> frozen = heap ImmutableList<int32>(src);
int64 n = frozen.count();          // 3
int32 second = frozen.get(1);      // 20
int32 at = frozen.indexOf(30);     // 2
boolean has = frozen.contains(40); // false
```

## Methods

| Signature | |
|---|---|
| `ImmutableList(ArrayList<T> src)` ⚑ | Build an immutable copy of `src`; the source is left untouched |
| `int64 count()` | Number of elements |
| `boolean isEmpty()` | `count() == 0` |
| `T get(int32 i)` | Element at `i` (0-based), or the type's zero value if out of range |
| `boolean contains(T v)` | True iff some element is `==`-equal to `v`; O(n) |
| `int32 indexOf(T v)` | Index of the first `==`-equal element, or -1 if none; O(n) |
| `#ArrayStream<T> stream()` | Heap-allocated `ArrayStream` walking all elements in order |

⚑ = `@EntryPoint`

## See also

- Tour: [ImmutableListDemo](../../../samples/tour/src/main/cajeta/tour/collection/ImmutableListDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/ImmutableList.cajeta`](../../../runtime/src/cajeta/collection/ImmutableList.cajeta)
- [ArrayList](ArrayList.md) — the mutable source
- [ImmutableSet](ImmutableSet.md), [ImmutableMap](ImmutableMap.md) — the other frozen collections
