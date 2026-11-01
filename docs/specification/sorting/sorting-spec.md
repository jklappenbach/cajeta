# cajeta sorting — spec (the comparison/order seam, the host facility, the Tensor sort)

> **Status: spec (requirements + the load-bearing design decisions).** cajeta has no
> sorting facility today. It has only the `cajeta.lang.Comparable<T>` seam and the
> `<`/`>`-ordered trees and `Heap`. This spec defines three tightly coupled pieces that
> share one comparison and search abstraction:
> - **§4a** a general-purpose host sorting facility in `cajeta.collection`, which is
>   broadly useful because it sorts any `ArrayList` and backs `Stream.sorted()` and
>   `binarySearch`,
> - **§4b** the `Tensor` numeric sort in `cajeta.math`, which is data-parallel and
>   GPU-lowered,
> - **§4c** the shared seam, so that §4b reuses §4a and never forks it.
>
> It is derived from `collections-sorting-reuse-report.md`. The companion is
> `sorting-plan.md`. The Tensor pieces feed `numpy-porting-plan.md` Phase 7.

## 1. Scope & role
This is a framework-neutral, CPU-first, GPU-accelerable sorting capability. The host
facility (§4a) is ordinary eager stdlib in `cajeta.collection`, and it does not depend on
`cajeta.math` or any GPU. The Tensor sort (§4b) is data-parallel over dense typed
`Storage`/`KernelBuffer` and lowers to `cajeta.xpu` with the host facility as its CPU floor.
The two meet at one comparison and search seam (§4c). v1 covers the kernel-expressible sort
and search surface of numpy §3.7 plus the general host sort the language is missing. It is
not a full order-statistics library, and median and quantile interpolation stay in
`cajeta.math.stats`.

## 2. The load-bearing design decisions

### 2.1 The comparison/order seam — `Comparator<T>` + `Ordering`, atop existing `Comparable<T>` (RESOLVED)
There is one ordering abstraction, defined once in §4a and consumed everywhere, meaning
§4b, the ordered collections, and future code. `cajeta.lang.Comparable<T>`
(`int32 compareTo(T)`) already exists and is the natural order. This spec adds the missing
combinator layer:
- **`Comparator<T>`** is `int32 compare(T a, T b)`, with the same total, transitive and
  sign-symmetric contract as `compareTo`.
- **`Ordering`** combinators are `naturalOrder<T>()`, which delegates to `Comparable`,
  `reverseOrder<T>()`, `reversed()`, `thenComparing(Comparator<T>)`,
  `comparingBy(keyFn, keyComparator)`, and `nullsFirst`/`nullsLast`.
- **Hot-path rule:** numeric `Tensor` sorts (§4b) compare with dtype-specialized inline
  comparisons or radix key transforms. They never make a `Comparator` call per element,
  because the seam is the API contract and never a per-element indirection on the
  data-parallel path. Object and struct element sorts go through `Comparator`/`Comparable`.

### 2.2 Algorithm set + stability — numpy `kind=` parity (RESOLVED)
Expose the numpy `kind` contract so §4a and §4b promise the same semantics:
- **`quicksort` (default)** is an introsort, which is quicksort with a heapsort fallback on
  bad recursion and insertion sort for small runs. It is unstable, in-place, and O(n log n)
  worst-case. It is the primitive and dtype default.
- **`stable` / `mergesort`** is a stable merge sort, and Timsort-class run-merging is
  acceptable. It allocates O(n). It is the object default and serves any explicit stable
  request.
- **`heapsort`** is optional, reusing `Heap`'s sift. It is unstable and in-place, and it is
  low priority.
The chosen `kind` is part of each op's signature, and the stability guarantee is documented
per `kind`. This matches numpy, where only `stable`/`mergesort` are stable.

### 2.3 Return shapes (RESOLVED)
- Host: `sort` returns a new sorted `ArrayList<T>`, `sortInPlace` mutates, and
  `ArrayList.sort(...)` is in-place sugar. `Stream.sorted(...)` is a stateful, sequential
  terminal-feeding wrapper, and it rejects parallel.
- Tensor: `sort(axis, kind)` returns a sorted `Tensor<T>`, and `argsort` returns a
  `Tensor<int64>` index permutation. `partition(kth)`/`argpartition` give the
  k-th-element guarantee with numpy semantics, and `lexsort(keys)` returns the index
  permutation of a stable multi-key sort.

### 2.4 Search — one implementation (RESOLVED)
`binarySearch` / `lowerBound` / `upperBound` run over a sorted `ArrayList<T>` and raw `T[]`,
parameterized by the §2.1 order. This single implementation is also numpy `searchsorted`,
where `side='left'` is lowerBound and `'right'` is upperBound, and it is the bin lookup for
`histogram`/`digitize` with non-uniform edges.

## 3. §4a — the general host sorting facility (`cajeta.collection`, eager)
This is the missing stdlib piece, and it is useful independent of numpy.
- **Types/functions:** `Comparator<T>`, `Ordering` (§2.1); `sort<T>(ArrayList<T>, …)`,
  `sortInPlace<T>(...)`, `ArrayList.sort(...)`; `binarySearch`/`lowerBound`/`upperBound`
  (§2.4); `Stream.sorted(...)`; `Collectors.toSortedList(...)`.
- **Algorithms:** introsort (§2.2 unstable) and stable mergesort (§2.2), with an
  insertion-sort cutoff for small runs. It is CPU-only, because this is the host floor.
- **Element types:** primitives and dtypes today. Class element types need
  ordering-through-template specialization, which is a current language limitation. It
  works for `Comparable` classes via monomorphic dispatch, and the general
  templated-`Comparator` path over an arbitrary class `T` is the known gap (§7).

## 4. §4b — the `Tensor` numeric sort (`cajeta.math`, Phase 7)
This is data-parallel sort and search over dense typed buffers. It lives outside the
general collections.
- **Surface:** `sort`/`argsort` (along `axis`, `kind=`), `partition`/`argpartition`,
  `lexsort`, `searchsorted`, and the sort-backed `unique`. The reuse report
  cross-references this, since GPU `unique` is sort plus adjacent-difference plus
  scan-compaction.
- **CPU floor:** it reuses §4a. Introsort or mergesort runs over each 1-D lane along
  `axis`, `argsort` sorts an index vector, `partition` is quickselect using introsort's
  partition, `lexsort` is a stable sort by successive keys, and `searchsorted` is the
  §2.4 bounds.
- **GPU path:** radix sort for integer and float keys via an order-preserving key
  transform (§6), built on `Wave.prefixSum` for the per-digit histogram and scan, plus
  `Shared<T>` and `Barrier`. Bitonic sort serves within-tile and small-N cases and is the
  merge network. `searchsorted` is a binary-search kernel, and `partition` is
  radix-select. Results are cross-checked against the CPU floor, bit-exact for integer and
  exact types, under the existing portable-versus-native discipline.
- **Axis/stability:** sort is along one `axis`, and the others are independent lanes.
  `kind=` is honored. Radix is naturally stable, so it backs `stable`/`lexsort`, and
  bitonic is unstable, so it backs `quicksort`.

## 5. §4c — the shared seam (the point)
The comparison and order abstraction (§2.1) and `binarySearch`/`lowerBound`/`upperBound`
(§2.4) are defined once in §4a. §4b consumes them. The Tensor CPU floor is the §4a sort, one
search implementation serves `searchsorted`/`histogram`/`unique`-merge, and the GPU
radix/bitonic is the only genuinely new code and never touches `Comparator` on the hot path.
There is no duplication of comparison or search logic anywhere.

## 6. dtype key transforms (the radix enabler)
Radix sorts unsigned integers. To radix-sort signed ints and floats with correct order:
- **signed int:** flip the sign bit, which biases by 2^(n-1).
- **IEEE float:** if the sign bit is set, flip all bits, and otherwise flip only the sign
  bit. This gives an order-preserving unsigned key that handles ±0 and denormals, and NaN
  sorts last, matching numpy.
- Reverse the transform after sorting to recover values. The transform is the only
  dtype-specific knowledge the GPU radix path needs, and comparisons elsewhere use `<` on
  the native dtype.

## 7. Goals / Non-goals
**Goals:** the `Comparator`/`Ordering` seam and `binarySearch`/bounds (§2.1/§2.4), the
host introsort, stable mergesort and `ArrayList.sort`/`Stream.sorted` (§4a), the Tensor
`sort`/`argsort`/`partition`/`argpartition`/`lexsort`/`searchsorted` CPU floor (§4b) and
GPU radix/bitonic, cross-checked, numpy `kind=` parity, and the float/signed key transform
(§6).
**Non-goals (v1):** median, quantile and percentile interpolation, which go to
`cajeta.math.stats`; set ops beyond sort-backed `unique`, which go to the reuse report and
the stats-adjacent work; a fully templated `Comparator` sort over arbitrary class element
types, which is gated on the operator/ordering-through-template limitation, while
primitives, dtypes and `Comparable` classes work in v1; and GPU sort of non-numeric or
struct dtypes, which stays on the CPU floor only.

## 8. Acceptance criteria
1. `Comparator`/`Ordering` + `binarySearch`/`lowerBound`/`upperBound` exist and match an
   oracle across the order combinators (natural/reverse/then/by/nulls).
2. Host `sort` is correct for both `kind`s. `stable` is verifiably stable, and `quicksort`
   verifiably sorts and is fast, with the introsort worst case bounded. `ArrayList.sort` /
   `Stream.sorted` / `Collectors.toSortedList` work.
3. Tensor `sort`/`argsort`/`partition`/`argpartition`/`lexsort`/`searchsorted` match a numpy
   oracle on the canonical cases (along `axis`, each `kind`, NaN-last), on a no-GPU build.
4. The GPU radix/bitonic path agrees with the CPU floor, bit-exact for int and exact types.
   Tolerance does not apply to a sort, because it is a permutation. The float/signed key
   transform round-trips order-correctly, including ±0 and NaN-last.
5. §4b shares §4a's comparison and search code, with no forked comparator or search, and
   `searchsorted` is literally the §2.4 bounds.
