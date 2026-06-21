---
id: collection-Sort
applies-to: [cajeta/collection/Sort]
title: Sort — host array sort/search and the shared comparator seam
description: Static sort/sortStable/lowerBound/upperBound/binarySearch over (T[], n) with a (T,T)->int32 comparator or natural-order overloads.
---

# Sort — host array sorting & searching

The stdlib host sort and the shared comparison/search **seam** the `Tensor`
numeric sort reuses. Use it to order or search a raw `T[]`:

| You want to… | Call |
|---|---|
| fast in-place sort, order of equal keys irrelevant | `Sort.sort<T>(a, n)` — unstable quicksort |
| in-place sort that keeps equal keys in input order | `Sort.sortStable<T>(a, n)` — stable merge sort |
| first index not ordering before `key` (numpy `searchsorted` left) | `Sort.lowerBound<T>(a, n, key)` → `[0, n]` |
| first index ordering after `key` (numpy `searchsorted` right) | `Sort.upperBound<T>(a, n, key)` → `[0, n]` |
| index of `key`, or absent | `Sort.binarySearch<T>(a, n, key)` → index or `-1` |
| custom / descending / sort-by-field order | pass a 4th arg: `(T, T) -> int32 cmp` |
| sort an `ArrayList<T>` | not here — `xs.sort()` / `xs.sortStable()` instance methods |
| argsort / partial sort / nth-element / dedup | not provided |

`Sort` is a **static utility class** — you never construct it; there is no
instance and no state. Just `import cajeta.collection.Sort;` and call.

## The comparator seam

Every algorithm is implemented once against a comparator function value
`(T, T) -> int32`: return **negative** if the first arg orders before the
second, **zero** if equal, **positive** if after (same contract as
`cajeta.lang.Comparable` — total, transitive, sign-symmetric). The
natural-order overloads (no `cmp` arg) are thin wrappers that pass a `<`/`>`
comparator. Because they rely on the `<`/`>` operators, primitive `T`
(`int32`, `float32`, …) works today; **class `T` works only once operator
overloading flows through template specialization** (the same v1 limit `Heap`
and `RedBlackTree` note) — until then, pass an explicit comparator for class
types.

## Ownership & lifecycle

- `a` is **borrowed, mutated in place**. `sort`/`sortStable` reorder the
  caller's array; no `#` transfer, no ownership change, nothing returned. The
  caller still owns `a` afterward.
- Internal scratch is allocated and freed inside the call — `sort` uses a fixed
  `heap int32[128]` range stack (depth is O(log n)-bounded, so 128 is safe for
  any `n`); `sortStable` allocates a `heap T[n]` merge buffer, O(n) aux. Neither
  escapes; nothing for the caller to free.
- Search methods read `a` only and return an `int32` (a value, not a handle).

## Sharp edges

- **Search requires `a` already sorted by the same comparator** — these are
  binary searches, no validation. Sorting with one `cmp` then searching with a
  different ordering gives garbage indices.
- `binarySearch` signals "absent" with the sentinel **`-1`**, not an exception;
  on duplicate keys it returns the **first** match. `lowerBound`/`upperBound`
  return in `[0, n]` (can equal `n`).
- `n` is the element count; only `a[0..n)` is touched, so you can sort a prefix
  of a larger array.
- `sort` is **unstable** — for equal keys use `sortStable` if input order must
  be preserved.
- No bounds/null checks and no exceptions are raised; passing `n` larger than
  the array length is undefined.

## Example (mirrors test/collection/SortTests.cpp)

```cajeta
package app;

import cajeta.collection.Sort;

public final class Demo {
    public static int32 run() {
        int32[] a = { 5, 3, 8, 1, 9, 2, 7 };
        Sort.sort<int32>(a, 7);                       // [1,2,3,5,7,8,9] ascending
        int32 at = Sort.binarySearch<int32>(a, 7, 8); // 5
        int32 lb = Sort.lowerBound<int32>(a, 7, 5);   // 3

        // Descending via an explicit comparator (the seam, supplied per call):
        Sort.sort<int32>(a, 7, (x, y) -> {
            if (x > y) { return -1; }
            if (x < y) { return 1; }
            return 0;
        });                                           // [9,8,7,5,3,2,1]
        return at + lb;                               // 8
    }
}
```

See also `cajeta.lang.Comparable` (the ordering contract) and
`cajeta.collection.ArrayList` (whose `sort()`/`sortStable()` route through the
`(T[], int32)` entry points here).
