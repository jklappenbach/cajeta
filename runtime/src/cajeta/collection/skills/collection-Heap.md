---
id: collection-Heap
applies-to: [cajeta/collection/Heap]
title: Heap — array-backed binary min-heap / priority queue
description: Using cajeta.collection.Heap, a min-heap whose peek/pop return the smallest element by operator< on T.
---

# Heap — binary min-heap (smallest first)

`Heap<T>` is an array-backed binary **min-heap** / priority queue: `peek()` and
`pop()` return the **smallest** element by the `<` ordering on `T`, never the
largest. This is the sharp edge — if you want the largest out first, you must
**reverse the ordering** (wrap `T` in a type whose `operator<` is inverted);
there is no max-heap and no comparator parameter. It is an **entry-point** type:
you construct it and call it directly.

## Construct, push, drain ascending

```cajeta
import cajeta.collection.Heap;

Heap<int32> h = heap Heap<int32>();   // empty, initial capacity 16
h.push(5);
h.push(1);
h.push(3);

int32 lo = h.peek();        // 1  — smallest, NOT removed
int32 a  = h.pop();         // 1  — smallest, removed
int32 b  = h.pop();         // 3
int64 left = h.count();     // 1  (note: int64, not int32)
boolean done = h.isEmpty(); // false
```

Push in any order; repeated `pop` drains in ascending order.

## v1 surface (the whole API)

`push(T v)` / `peek() -> T` / `pop() -> T` / `count() -> int64` /
`isEmpty() -> boolean`. That is everything.

- `push` is `O(log n)` amortized (sift-up), `O(n)` on the rare grow; it doubles
  the backing array when full, same strategy as `cajeta.collection.ArrayList`.
- `pop` is `O(log n)` (sift-down of the moved last element).

## Ownership & lifecycle

- **`push(T v)` does not take ownership** of a value — there is no `#` on the
  parameter; the element is stored into the backing array by assignment. Pass
  primitives or the values you intend the heap to hold.
- **`peek` / `pop` return `T` by value** — no ownership transfer, nothing for the
  caller to free for primitive `T`.
- Construct with `heap Heap<T>()`; the heap owns its backing array. There is **no
  `close()` / `dispose()`** — reclamation follows normal cajeta drop. Construction
  takes no arguments and no capacity hint (it is fixed at 16, then doubles).

## Empty / miss semantics — does NOT throw

`peek()` and `pop()` on an **empty** heap return the type's **zero value** (the
stdlib miss-path convention), they do **not** throw and do **not** return null.
Because that zero value is indistinguishable from a real zero element, **guard
with `isEmpty()` / `count()`** before draining:

```cajeta
while (!h.isEmpty()) {
    int32 next = h.pop();   // safe — known non-empty
}
```

An exception-throwing variant is deferred until the error model lands.

## Ordering — the rule that gates T

Element order comes from the `<` operator on `T`, exactly like
`cajeta.lang.Math.min`/`max`.

- **Primitive `T`** (`int32`, `int64`, `float`, …) works today via built-in
  comparison.
- **Class `T`** needs an `operator<` overload, which does not yet link through
  template specializations (the v1 limitation `Math` documents) — so a class
  element compiles but won't order correctly until that lands.

## What it does NOT do

- **Not a max-heap** — no largest-first, no comparator/`reverse` option; invert
  `operator<` on a wrapper type instead.
- **No arbitrary removal / update / `contains` / `decreaseKey`** — only the top
  via `pop`.
- **No bounds/empty exceptions** — empty `peek`/`pop` return the zero value (see
  above).
- **No iterator / no ordered listing** — the only ordered output is repeated
  `pop`; the backing array is not in sorted order.
- **No bulk constructor / heapify** — build by repeated `push`.

See `cajeta.lang.Math` for the same `<` ordering convention and
`cajeta.collection.ArrayList` for the same doubling-array growth strategy.
