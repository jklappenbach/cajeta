---
id: nucleo-sparse-CsrMatrix
applies-to: [cajeta/nucleo/sparse/CsrMatrix, cajeta.nucleo.sparse]
title: CsrMatrix — float64 compressed-sparse-row matrix (+ CSC mirror for column consumers)
description: The stdlib sparse-matrix type — builders (fromDense/fromCoo with scipy's duplicate-sum rule), SpMV/SpMVT, the buildCsc() column-walk surface with cached norms, and what deliberately isn't here (no arithmetic operators, no other dtypes).
---

# cajeta.nucleo.sparse.CsrMatrix

Float64 CSR: `rowPtr (n+1)`, `colIdx (nnz)`, `values (nnz)`, columns
ascending within each row. Doctrine: the TYPE and its primitives live
here in the stdlib; algorithms that consume it live in external libraries
(first consumer: `dev.cajeta.ml`'s sparse coordinate descent, whose
sparse fit is bit-identical to its dense fit).

## Build

```cajeta
import cajeta.nucleo.sparse.CsrMatrix;

CsrMatrix a #= CsrMatrix.fromDense(dm);              // Tensor<float64> (n,p); nonzeros only
CsrMatrix b #= CsrMatrix.fromCoo(rows, cols, vals,   // int64[]/int64[]/float64[]
                                nnz, n, p);         // any order; DUPLICATES SUM (scipy's rule)
```

- Builders COPY their inputs (the `Column.of` convention) — your arrays
  stay yours.
- `fromCoo` sorts per-row and merges duplicates by summing; an
  exact-zero SUM keeps its slot (no re-sparsification). Out-of-range
  indices throw `SparseException` (a `RecoverableException`).
- `fromDense` stores only `!= 0.0` entries.

## Use

| Operation | Method | Shape contract |
|---|---|---|
| dims / stored count | `rowCount()`, `colCount()`, `nnz()` | — |
| densify | `toDense()` | `(n, p)` Tensor |
| `A·x` | `matVec(x)` | `(p,)` in → `(n,)` out |
| `Aᵀ·x` | `matVecT(x)` | `(n,)` in → `(p,)` out |
| column walks | `buildCsc()` ONCE, then `colStart(j)`/`colEnd(j)` slots, `cscRowAt(t)`, `cscValAt(t)` | rows ascending within a column |
| cached `‖col j‖²` | `colNorm2(j)` | requires `buildCsc()` |

`buildCsc()` is idempotent; calling any `col*`/`csc*` accessor before it
throws `SparseException("… call buildCsc() first")`.

The coordinate-descent inner loop this surface was shaped for:

```cajeta
x.buildCsc();
int64 t = x.colStart(j);
while (t < x.colEnd(j)) {
    int64   r = x.cscRowAt(t);
    float64 v = x.cscValAt(t);
    resid[r] = resid[r] + wj * v;      // axpy over one column's nonzeros
    t = t + 1;
}
```

## Not here (dead-end avoidance)

- **No arithmetic operators, no sparse-sparse ops** (no `+`, no SpGEMM),
  no factorizations — v1 is storage + SpMV + column access.
- **float64 only** — no other dtypes, no complex.
- No CSC-first constructor: build CSR, call `buildCsc()`.
- No implicit dense fallback: `matVec` on wildly wrong shapes throws, it
  never densifies behind your back.
- Mutation: none after construction (rebuild from COO/dense) — the type
  is effectively frozen once built; `buildCsc` only ADDS the mirror.
