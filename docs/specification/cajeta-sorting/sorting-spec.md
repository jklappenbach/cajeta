# cajeta sorting — spec (the comparison/order seam, the host facility, the Tensor sort)

> **Status: spec (requirements + the load-bearing design decisions).** cajeta has **no
> sorting facility** today — only the `cajeta.lang.Comparable<T>` seam and `<`/`>`-ordered
> trees/`Heap`. This spec defines three tightly-coupled pieces that share ONE comparison/
> search abstraction:
> - **§4a** a general-purpose **host sorting facility** in `cajeta.collection` (broadly
>   useful — sort any `ArrayList`, `Stream.sorted()`, `binarySearch`),
> - **§4b** the **`Tensor` numeric sort** (`cajeta.math`, data-parallel, GPU-lowered),
> - **§4c** the **shared seam** so §4b reuses §4a rather than forking it.
>
> Derived from `collections-sorting-reuse-report.md`. Companion: `sorting-plan.md`. The
> Tensor pieces feed `numpy-porting-plan.md` Phase 7.

## 1. Scope & role
A framework-neutral, **CPU-first**, GPU-accelerable sorting capability. The host facility
(§4a) is ordinary eager stdlib (`cajeta.collection`) — it does not depend on `cajeta.math`
or any GPU. The Tensor sort (§4b) is data-parallel over dense typed `Storage`/`KernelBuffer`
and lowers to `cajeta.xpu` with the host facility as its CPU floor. The two meet at one
comparison/search seam (§4c). v1 covers the **kernel-expressible** sort/search surface of
numpy §3.7 plus the general host sort the language is missing; it is *not* a full
order-statistics library (median/quantile interpolation stays in `cajeta.math.stats`).

## 2. The load-bearing design decisions

### 2.1 The comparison/order seam — `Comparator<T>` + `Ordering`, atop existing `Comparable<T>` (RESOLVED)
One ordering abstraction, defined once (§4a), consumed everywhere (§4b, the ordered
collections, future code). `cajeta.lang.Comparable<T>` (`int32 compareTo(T)`) already exists
and is the *natural order*; this adds the missing **combinator** layer:
- **`Comparator<T>`** — `int32 compare(T a, T b)` (the same total/transitive/sign-symmetric
  contract as `compareTo`).
- **`Ordering`** combinators: `naturalOrder<T>()` (delegates to `Comparable`),
  `reverseOrder<T>()`, `reversed()`, `thenComparing(Comparator<T>)`,
  `comparingBy(keyFn, keyComparator)`, `nullsFirst`/`nullsLast`.
- **Hot-path rule:** numeric `Tensor` sorts (§4b) compare with **dtype-specialized inline
  comparisons** (or radix key transforms), *not* a `Comparator` call per element — the seam
  is the API/contract, not a per-element indirection on the data-parallel path. Object/struct
  element sorts go through `Comparator`/`Comparable`.

### 2.2 Algorithm set + stability — numpy `kind=` parity (RESOLVED)
Expose the numpy `kind` contract so §4a and §4b promise the same semantics:
- **`quicksort` (default)** — an **introsort** (quicksort + heapsort fallback on bad
  recursion; insertion sort for small runs). **Unstable**, in-place, O(n log n) worst-case.
  Primitive/dtype default.
- **`stable` / `mergesort`** — a **stable** merge sort (Timsort-class run-merging acceptable).
  Allocates O(n). Object default + any explicit stable request.
- **`heapsort`** — optional, reusing `Heap`'s sift; unstable, in-place. Low priority.
The chosen `kind` is part of each op's signature; the **stability guarantee is documented
per `kind`** (matches numpy: only `stable`/`mergesort` are stable).

### 2.3 Return shapes (RESOLVED)
- Host: `sort` returns a new sorted `ArrayList<T>`; `sortInPlace` mutates; `ArrayList.sort(...)`
  is in-place sugar; `Stream.sorted(...)` is a (stateful, sequential — rejects parallel)
  terminal-feeding wrapper.
- Tensor: `sort(axis, kind)` returns a sorted `Tensor<T>`; **`argsort`** returns a
  `Tensor<int64>` index permutation; `partition(kth)`/`argpartition` give the
  k-th-element guarantee (numpy semantics); `lexsort(keys)` returns the index permutation of
  a stable multi-key sort.

### 2.4 Search — one implementation (RESOLVED)
`binarySearch` / `lowerBound` / `upperBound` over a sorted `ArrayList<T>` and raw `T[]`,
parameterized by the §2.1 order. This single implementation is **also** numpy
`searchsorted` (`side='left'`→lowerBound, `'right'`→upperBound) and the bin lookup for
`histogram`/`digitize` (non-uniform edges).

## 3. §4a — the general host sorting facility (`cajeta.collection`, eager)
The missing stdlib piece; useful independent of numpy.
- **Types/functions:** `Comparator<T>`, `Ordering` (§2.1); `sort<T>(ArrayList<T>, …)`,
  `sortInPlace<T>(...)`, `ArrayList.sort(...)`; `binarySearch`/`lowerBound`/`upperBound`
  (§2.4); `Stream.sorted(...)`; `Collectors.toSortedList(...)`.
- **Algorithms:** introsort (§2.2 unstable) + stable mergesort (§2.2). Insertion-sort
  cutoff for small runs. CPU-only (this is the host floor).
- **Element types:** primitives/dtypes today; **class element types need
  ordering-through-template specialization** (a current language limitation) — works for
  `Comparable` classes via monomorphic dispatch; the general templated-`Comparator` path over
  arbitrary class `T` is the known gap (§7).

## 4. §4b — the `Tensor` numeric sort (`cajeta.math`, Phase 7)
Data-parallel sort/search over dense typed buffers, **not** in general collections.
- **Surface:** `sort`/`argsort` (along `axis`, `kind=`), `partition`/`argpartition`,
  `lexsort`, `searchsorted`, and the sort-backed `unique` (cross-refs the reuse report:
  GPU `unique` = sort + adjacent-difference + scan-compaction).
- **CPU floor:** **reuses §4a** — introsort/mergesort over each 1-D lane along `axis`;
  `argsort` sorts an index vector; `partition` = quickselect (introsort's partition);
  `lexsort` = stable sort by successive keys; `searchsorted` = §2.4 bounds.
- **GPU path:** **radix sort** for integer/float keys via an **order-preserving key
  transform** (§6), built on `Wave.prefixSum` (per-digit histogram + scan) + `Shared<T>` +
  `Barrier`; **bitonic** for within-tile/small-N and as the merge network; `searchsorted` =
  a binary-search kernel; `partition` = radix-select. **Result-cross-checked** against the
  CPU floor (bit-exact for integer/exact; the existing portable-vs-native discipline).
- **Axis/stability:** sort is along one `axis` (others are independent lanes); `kind=`
  honored (radix is naturally stable → backs `stable`/`lexsort`; bitonic is unstable →
  `quicksort`).

## 5. §4c — the shared seam (the point)
The comparison/order abstraction (§2.1) and `binarySearch`/`lowerBound`/`upperBound` (§2.4)
are defined **once** in §4a. §4b consumes them: the Tensor CPU floor *is* the §4a sort; one
search implementation serves `searchsorted`/`histogram`/`unique`-merge; the GPU radix/bitonic
is the only genuinely new code and never touches `Comparator` on the hot path. No duplication
of comparison or search logic anywhere.

## 6. dtype key transforms (the radix enabler)
Radix sorts unsigned integers; to radix-sort signed ints and floats with correct order:
- **signed int:** flip the sign bit (bias by 2^(n-1)).
- **IEEE float:** if sign bit set, flip all bits; else flip only the sign bit → an
  order-preserving unsigned key (handles ±0, denormals; **NaN sorts last**, matching numpy).
- Reverse the transform after sorting to recover values. The transform is the only
  dtype-specific knowledge the GPU radix path needs; comparisons elsewhere use `<` on the
  native dtype.

## 7. Goals / Non-goals
**Goals:** the `Comparator`/`Ordering` seam + `binarySearch`/bounds (§2.1/§2.4); the host
introsort + stable mergesort + `ArrayList.sort`/`Stream.sorted` (§4a); the Tensor
`sort`/`argsort`/`partition`/`argpartition`/`lexsort`/`searchsorted` CPU floor (§4b) +
GPU radix/bitonic, cross-checked; numpy `kind=` parity; the float/signed key transform (§6).
**Non-goals (v1):** median/quantile/percentile interpolation (→ `cajeta.math.stats`); set
ops beyond sort-backed `unique` (→ reuse report / stats-adjacent); a fully generic
templated-`Comparator` sort over arbitrary **class** element types (gated on the
operator/ordering-through-template limitation — primitives/dtypes + `Comparable` classes
work in v1); GPU sort of non-numeric/struct dtypes (CPU floor only).

## 8. Acceptance criteria
1. `Comparator`/`Ordering` + `binarySearch`/`lowerBound`/`upperBound` exist and match an
   oracle across the order combinators (natural/reverse/then/by/nulls).
2. Host `sort` is correct for both `kind`s; **stable** is verifiably stable, **quicksort**
   verifiably sorts (and is fast — introsort worst-case bounded); `ArrayList.sort` /
   `Stream.sorted` / `Collectors.toSortedList` work.
3. Tensor `sort`/`argsort`/`partition`/`argpartition`/`lexsort`/`searchsorted` match a numpy
   oracle on the canonical cases (along `axis`, each `kind`, NaN-last), on a no-GPU build.
4. The GPU radix/bitonic path agrees with the CPU floor (bit-exact int/exact; tolerance n/a
   for sort — it's a permutation); the float/signed key transform round-trips order-correctly
   incl. ±0 and NaN-last.
5. §4b shares §4a's comparison + search code (no forked comparator/search); `searchsorted`
   is literally §2.4 bounds.
