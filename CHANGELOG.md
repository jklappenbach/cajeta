# Changelog

## 0.40.0 (2026-10-11)

- `int64` wave verbs: `Wave.reduceSumI64`, `reduceMaxI64`, `reduceMinI64`, `prefixSumI64` and the four `shuffle*SyncI64` forms.
- `Workgroup.reduce` takes `int64`; new `Workgroup.scan`, an exclusive scan over `int32` and `int64`.
- cpu backend: a local assigned under a per-lane branch and read after a barrier is now per work-item (it read the last work-item's value).
- cpu backend: barrier fission fixes for `break` out of a uniform loop, zero-initialized uniform slots, and the global index over a pinned block.
- `@Unlowered(hold = true)` skips a kernel on every backend; a class template keeps an interface declared in a later file.
