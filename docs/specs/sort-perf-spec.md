# Spec: competitive unstable sort (`Sort.sort`)

## 1. Definition

### 1.1 Purpose
`Sort.sort` (the unstable quicksort behind `sort-int64` / `sort-f64`) should be
competitive with the best in-language unstable sorts (C++ `pdqsort`/`std::sort`, Rust
`sort_unstable`, Go `slices.Sort`) across the standard input patterns — not catastrophically
slow on any of them.

### 1.2 Problem
The current sort is a Lomuto-partition quicksort with a middle-element pivot, no
duplicate handling, and no pattern detection. Measured at n=50000 (median):
- **dups (`i%100`): 16.7ms — 214× off C++ pdqsort (0.078ms).** Lomuto degrades to O(n^2)
  on many equal keys (everything equal to the pivot piles on one side).
- **ascending: 0.86ms, descending: 1.05ms** — 4–43× off the leaders. Already-sorted
  input still pays full quicksort cost (no short-circuit).
- **random: ~2.8ms** — ~1.5× off `std::sort`, ~3.7× off `pdqsort`; the baseline
  partition/pivot is just slower than an optimized one.

### 1.3 Scope
`runtime/src/cajeta/collection/Sort.cajeta` — `sort<T>` only. Apply the well-known
pdqsort/introsort techniques: 3-way partitioning, median-of-3 (ninther for large)
pivot, and a nearly-sorted fast path. `sortStable`, `binarySearch`, `lowerBound`,
`upperBound` are out of scope (already competitive). The O(log n) explicit range stack
(recurse-smaller / loop-larger) is retained.

### 1.4 Non-goals
- 1.4.1 A full block-quicksort (pdqsort's branchless block partition) — a stretch goal,
  not required to close the order-of-magnitude gaps.
- 1.4.2 Eliminating the comparator-closure call overhead (a separate constant-factor
  concern, possibly a compiler change) — tracked separately if the algorithmic wins
  leave a gap.
- 1.4.3 Changing the public API or the stable/search functions.

## 2. Feature: pattern-defeating unstable sort

### 2.1 Use cases
- 2.1.1 As a developer, sorting **many-duplicate** data (e.g. `i%100`) is O(n log n),
  not O(n^2): `dups` drops from ~16.7ms to a small multiple of the other patterns
  (target: well under 1ms — within ~10× of pdqsort, ideally better than std::sort 0.5ms).
- 2.1.2 As a developer, **already-sorted** (ascending) input is detected and finishes far
  faster than a full sort (target: ≤ ~0.1ms, ≥ 8× faster than today's 0.86ms).
- 2.1.3 As a developer, **reverse-sorted** (descending) input is likewise fast.
- 2.1.4 As a developer, **random** input sorts at least as fast as today and ideally
  approaches `std::sort` (target: ≤ ~1.9ms, from ~2.8ms).
- 2.1.5 As a developer, every pattern still produces a correct non-decreasing permutation
  (guarded by `SortAdversarialTests`), and the range stack stays O(log n) (no overflow).
- 2.1.6 As a developer, the same improvements carry to `sort-f64` (float64 element type)
  and any `sort<T>` element/comparator.

## 3. Acceptance themes
- 3.1 Correctness: `SortAdversarialTests` (ascending/descending/dups/random/organ-pipe)
  stay green; the sort is a true permutation, no overflow.
- 3.2 Perf: `sort-int64` dups ≪ 1ms, ascending/descending ≤ ~0.1ms, random ≤ today;
  `sort-f64` improved; all `check=true`.
- 3.3 Non-regression: `sortStable` / `binarySearch` unchanged; full suite + site updated.
