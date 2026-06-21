# Spec: `Sort.sort` robustness on adversarial patterns (sort-int64)

## 1. Definition

### 1.1 Purpose
`Sort.sort` (the unstable quicksort) must correctly and safely sort all input patterns
the `sort-int64` benchmark targets — random, ascending, descending, and
many-duplicates — at scale (n = 50000), so the benchmark can measure every pattern
instead of skipping the adversarial ones.

### 1.2 Problem
`SortInt64Bench` currently runs only the `random` variant and **skips** ascending /
descending / dups, with `skipReason` "Cajeta Sort quicksort overflows its 128-deep
range stack on adversarial patterns (no introsort guard)". The skip records the
adversarial patterns as a coverage gap rather than data — effectively a partial failure
of `sort-int64`. The current implementation already loops the smaller partition and
pushes the larger (an O(log n) stack-depth bound) with a middle-element pivot, so the
overflow premise needs verification: either the adversarial patterns already sort
correctly (and the skip should be removed), or a real defect (stack growth, mis-sort)
remains and must be fixed.

### 1.3 Scope
- `runtime/src/cajeta/collection/Sort.cajeta` — `sort<T>` (range stack, pivot,
  partition) only if a real defect surfaces.
- `samples/profile/src/main/cajeta/profile/sort/SortInt64Bench.cajeta` — variant
  support + skip reason.

### 1.4 Non-goals
- 1.4.1 Stable sort / float sort changes (`sortStable`, `SortF64Bench`).
- 1.4.2 Switching the algorithm family (e.g. full introsort with heapsort fallback)
  unless verification shows the explicit-stack quicksort genuinely overflows.

## 2. Feature: correct + safe adversarial sorting

### 2.1 Use cases
- 2.1.1 As a developer, sorting an **ascending** int64 array of 50000 elements yields a
  non-decreasing array with no crash / stack overflow.
- 2.1.2 As a developer, the same for a **descending** array.
- 2.1.3 As a developer, the same for a **many-duplicates** array (`i % 100`).
- 2.1.4 As a developer, the same for **random** (unchanged from today).
- 2.1.5 As a developer, the sort is a true permutation of the input (no lost/duplicated
  elements) for each pattern.
- 2.1.6 As a maintainer, `sort-int64` measures all four patterns (no skip) with
  `check=true`, OR — if a genuine overflow is confirmed — the implementation is fixed so
  it doesn't, and only then is the skip removed.

## 3. Acceptance themes
- 3.1 Correctness: 2.1.1-2.1.5 sort correctly at n = 50000 with no crash.
- 3.2 Coverage: the benchmark runs all four patterns with `check=true` (skip removed),
  justified by the tests.
- 3.3 Non-regression: `random` performance and other sort benchmarks are unaffected.
