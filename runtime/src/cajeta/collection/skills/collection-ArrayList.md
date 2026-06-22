---
id: collection-ArrayList
applies-to: [cajeta/collection/ArrayList]
title: ArrayList — growable T[]-backed sequence
description: cajeta's default mutable list — construction, get/set/add/appendAll, stream/sort, and the array-ownership trap on grow.
---

# ArrayList&lt;T&gt;

The workhorse growable, index-addressable sequence in `cajeta.collection`, backed by a
heap `T[]` that **doubles** on demand. This is the **go-to mutable list** and the default
accumulator behind `Collectors.toList<T>()`. Reach for it whenever you need an ordered,
append-and-index collection. Need an immutable snapshot instead → `ImmutableList`; need
key→value or membership → `HashMap` / `HashSet`.

**Access point:** yes — you construct and drive it directly.

## Construction & ownership

No-arg constructor only; allocates a backing `T[]` of initial capacity 16. Heap-allocate it.

```cajeta
import cajeta.collection.ArrayList;

ArrayList<int32> xs = heap ArrayList<int32>();
xs.add(10);
xs.add(20);
xs.add(30);                       // count() == 3
int32 first = xs.get(0);          // 10
xs.set(1, 25);                    // [10, 25, 30]
int32 sum = xs.stream().reduce(0, (a, b) -> a + b);   // 55
```

`T` may be primitive (`ArrayList<int32>`) or class-typed (`ArrayList<Point>` holding
`heap Point(...)` elements — `add` stores the caller's same instances, no `#` transfer).

## Methods that matter

- `int32 count()` — live element count. **The size accessor is `count()`, not `size()`** —
  the field is named `sizeCount` precisely to avoid colliding with this method.
- `boolean isEmpty()` — `count() == 0`.
- `T get(int32 i)` / `void set(int32 i, T v)` — slot read/write. Returns/stores by `T`'s
  normal value/reference convention (borrowed for class `T`; copy the element if you need
  to outlive the list).
- `void add(T v)` — append, growing (doubling) the backing array first if full.
- `void appendAll(ArrayList<T> other)` — append every element of `other` in order. **Does
  NOT consume `other`** — the caller still owns its list afterward. This is the combiner
  `Collectors.toList<T>()` uses to fold parallel partials.
- `#ArrayStream<T> stream()` — heap `ArrayStream` over `data[0..count-1]`; **ownership
  transfers to the caller** (the `#` return), who drives and drops it. Import
  `cajeta.lang.stream.ArrayStream` (or `Stream`) to hold it. Snapshot over current
  contents — do not mutate the list while a stream is live.
- `void sort()` — in-place ascending natural order (`<` on `T`), unstable quicksort.
- `void sortStable()` — in-place ascending, stable (merge sort; equal elements keep input
  order). Both delegate to `cajeta.collection.Sort` and touch only `data[0..count-1]`;
  trailing spare capacity is untouched. No copy.

## The one sharp edge — array ownership on grow

`add`'s grow path is the only ownership subtlety, and it is handled internally: the fresh
array is installed with `this.data = #grown` so the local's scope-exit drop does not free
storage still referenced by the field. You inherit this only if you write similar
manual-array code; calling `add` is safe.

## Lifecycle & state

Plain heap object — drops under normal scope/ownership rules. There is **no** `close()`
and no manual free. Mutable and **not** thread/fiber-safe; under `.parallel().collect(...)`
each worker gets its own partial `ArrayList` and they are merged via `appendAll`, so the
shared-mutation hazard never arises through that path.

## What it does NOT do (v1)

No bounds checks on `get`/`set`/`add` — an out-of-range index reads/writes raw backing
storage (bounds checks land with the error-model wiring). No `insert`, no `remove`, no
`clear`, no `indexOf`/`contains`, no `size()`. Reach for those only after confirming
they've been added; today the surface is exactly: ctor, `count`/`isEmpty`, `get`/`set`,
`add`, `appendAll`, `stream`, `sort`/`sortStable`.

See `cajeta.lang.stream` skills for the `ArrayStream`/`Stream` terminal surface
(`reduce`/`fold`/`map`/`collect`), `cajeta.collection.Sort` for the comparator-taking sort
overloads, and `cajeta.collection.Collectors` for `toList<T>()`.
