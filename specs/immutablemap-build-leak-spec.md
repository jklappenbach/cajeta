# immutablemap-build-leak — building an `ImmutableMap` from a map leaks, and each build is slower than the last (defect spec)

> Status: **draft**, filed 2026-10-26 by the benchmark-reference consolidation
> pass on v0.46.3 (post-rebuild compiler).

## 1. Definition
`collection/immutablemap-build` (freeze a 1e6-entry `HashMap` into an
`ImmutableMap`, 10 times a trial) takes 51 s, then 144 s, then 222 s on three
consecutive trials in one process, with resident memory rising from 1.4 GB
to 3.5 GB. Inside a whole-package run it passed an hour; alone, early in the
day, the package finished in 498 s. The benchmark builds through
`entries().forEach` into fresh pairs because building from
`entries().collect(toList())` throws (the collected pairs are borrows); the
growth says the frozen snapshot, the pairs, or the source map's entries are
not freed between builds, and the slowdown says the live-allocation set is
growing with them.

## 2. Use cases
- **2.1** As a program, when I freeze a map repeatedly, then each build costs
  the same and nothing accumulates: the snapshot owns its storage, the
  pairs are dropped, and `Cajeta.liveCount()` returns to its value before
  the build.
- **2.2** As the test suite, then a test freezes a 1e5-entry map 20 times
  and asserts a flat `liveCount` and a flat wall time; and a factory that
  copies from a borrowed map exists so the benchmark can use it.

## 3. Acceptance
`collection/immutablemap-build` runs its 10 builds a trial at a flat cost and
the smoke's known-gap skip is removed.
