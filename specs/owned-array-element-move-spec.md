# owned-array-element-move — `#arr[i]` compiles as a move of a borrow and double-frees

## 1. Definition

Found 2026-08-05 implementing ml-classification-gaps U2 (cajeta-ml
`LinearDiscriminantAnalysis.covOf`). Moving an element out of an owned array
of class-typed values compiles silently and produces a double-free at drop
time:

```cajeta
Tensor<float64>[] lw = Gaussians.ledoitWolf(blk);   // owned array
Tensor<float64> cov = lw[0];                        // borrow of element 0
return #cov;                                        // move of the borrow — ACCEPTED
// caller drops the returned tensor; lw's drop frees element 0 again → SIGSEGV
```

The scalar-local equivalent is correctly rejected with
`CAJETA_ERROR_MOVE_OF_BORROW`. The array-element path misses the check: the
element load produces a borrow, and `#` on it should be an error (the array
still owns the element and will drop it).

## 2. Requirements

- **2.1** When `#` is applied to a value loaded from an owned array element,
  the compile fails with `CAJETA_ERROR_MOVE_OF_BORROW` (or a dedicated
  diagnostic naming the array), matching the scalar-local behaviour.
- **2.2** When the diagnostic fires, it names the workaround: `.copy()` the
  element, or restructure so the array surrenders ownership as a whole.

## 3. Workaround (in use)

`Tensor<float64> cov = lw[0].copy(); return #cov;` — shipped in cajeta-ml
`LinearDiscriminantAnalysis` (commit e52db3a).

## 4. Reproduction

Any `#local` where `local = ownedArray[i]` of a class type; crash appears at
scope exit as a double drop (registry shows the same `obj` pointer twice).
