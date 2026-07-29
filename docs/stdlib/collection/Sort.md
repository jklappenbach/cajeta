# Sort

`cajeta.collection.Sort` — the general-purpose host sorting facility and the
shared comparison/search seam that the `Tensor` numeric sort reuses. A
comparator is a function value `(T, T) -> int32` (negative / zero / positive,
the `Comparable` contract); every algorithm is implemented once against a
comparator, and the natural-order entry points are thin wrappers that pass a
`<`/`>`-based comparator. `sort` is unstable and fast (numpy
`kind='quicksort'`); `sortStable` is a stable merge sort (numpy
`kind='stable'`). `lowerBound` / `upperBound` / `binarySearch` search a sorted
`T[]` with the same seam.

```cajeta
int32[] a = { 5, 3, 8, 1, 9, 2, 7 };
Sort.sort<int32>(a, 7);                        // [1, 2, 3, 5, 7, 8, 9]
int32 at = Sort.binarySearch<int32>(a, 7, 8);  // 5
int32 lo = Sort.lowerBound<int32>(a, 7, 4);    // 3 — first index not before 4
```

## Methods

| Signature | |
|---|---|
| `static void sort<T>(T[] a, int32 n, (T, T) -> int32 cmp)` ⚑ | Unstable in-place sort of `a[0..n)` by `cmp` |
| `static void sortStable<T>(T[] a, int32 n, (T, T) -> int32 cmp)` ⚑ | Stable sort of `a[0..n)` by `cmp` |
| `static int32 lowerBound<T>(T[] a, int32 n, T key, (T, T) -> int32 cmp)` | First index in sorted `a[0..n)` whose element does not order before `key` |
| `static int32 upperBound<T>(T[] a, int32 n, T key, (T, T) -> int32 cmp)` | First index in sorted `a[0..n)` whose element orders after `key` |
| `static int32 binarySearch<T>(T[] a, int32 n, T key, (T, T) -> int32 cmp)` | Index of `key` in sorted `a[0..n)`, or -1 if absent (first match on duplicates) |
| `static void sort<T>(T[] a, int32 n)` ⚑ | Natural-order unstable sort by `<` on `T` |
| `static void sort(int64[] a, int32 n)` | Concrete `int64` overload — the vectorized (AVX-512 vqsort) fast path; wins overload resolution over the template for a bare `Sort.sort(arr, n)` call |
| `static void sortStable<T>(T[] a, int32 n)` | Natural-order stable sort by `<` on `T` |
| `static int32 lowerBound<T>(T[] a, int32 n, T key)` | Natural-order `lowerBound` |
| `static int32 upperBound<T>(T[] a, int32 n, T key)` | Natural-order `upperBound` |
| `static int32 binarySearch<T>(T[] a, int32 n, T key)` | Natural-order `binarySearch` |

⚑ = `@EntryPoint`

The class also exposes a family of `pdq*` / `vq*` static helpers (partition,
sorting-network, and merge steps of the quicksort/mergesort implementation);
they are implementation machinery, not covered here.

## See also

- Source: [`runtime/src/cajeta/collection/Sort.cajeta`](../../../runtime/src/cajeta/collection/Sort.cajeta)
- [ArrayList](ArrayList.md) — `sort()` / `sortStable()` instance methods delegate here
- [Heap](Heap.md) — incremental priority ordering
- [Tensor](../math/Tensor.md) — the numeric sort built on the same seam
