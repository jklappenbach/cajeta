# Núcleo Sparse + Linear Algebra — Specification

> Status: draft for review (2026-06-23). These are two related surfaces under
> `dev.cajeta.nucleo`, sparse arrays (`dev.cajeta.nucleo.sparse`) and extended
> linear algebra (`dev.cajeta.nucleo.linalg`). Layer-1b. The companion analysis is
> `python-stack-analysis.md` §3.4 (SciPy), and the design context is `target-experience.md` §5.
> It builds on stdlib `cajeta.math.linalg` (which already ships `solve`/`det`/`inv`, with
> `qr`/`cholesky`/`lu`/`svd`/`eigh` as positional `Tensor[]` bags) and the
> `cajeta.math.Tensor`/GEMM substrate. Typed returns are records
> (`records-spec.md`) rather than positional tuples.
>
> This is a spec, so it states requirements and use cases, the why and the what. Build decisions,
> the how, are deferred as `> **TBD (plan-time):**` markers and collected in §10,
> to be resolved when this spec is turned into a plan. It is outline-numbered for
> addressability.
>
> **Re-scoped 2026-07-29** (python-stack-analysis §4.7, decided with Julian). This
> spec decomposes into three dispositions:
> 1. **RETAINED — the sparse array type** (§2–§4 core: CSR/CSC/COO over `Column`
>    buffers, COO construction, format conversion, sparse×dense and sparse×sparse
>    matmul). Types belong in núcleo, so this is still the spec of record. It is not yet
>    planned, since there is no commissioning consumer. The first likely trigger is sparse
>    design matrices in `cajeta-ml` (text/one-hot workloads).
> 2. **MOVED EXTERNAL — the sparse solvers** (`spsolve`, `cg`, any Krylov family +
>    preconditioners; TBDs [S5][S6] move with them). An algorithm family goes to a per-domain
>    external `cajeta-*` lib, commissioned on demand per the §4.7 rule.
> 3. **SUPERSEDED — the `dev.cajeta.nucleo.linalg` typed-record façade** (§7, TBDs
>    [L1]–[L3]). The stdlib keeps positional bags and is being completed directly by
>    `specs/linalg-solvers-spec.md` (rectangular QR/SVD, triangular + factor-
>    application solvers, consumer-grade lstsq, slogdet, norms). Typed-record
>    returns ride the external `dev.cajeta.scipy` façade instead.
> Reference source pinned: scipy v1.18.0 at `code/ml/scipy-ref`.

## 1. Definition

### 1.1 Purpose
SciPy is "numpy + algorithms over the array", and it adds exactly one new core
type, sparse (analysis §3.4). This spec covers that one new type and the linear
algebra that consumes it, in two coupled pieces:

- **Sparse arrays** (`dev.cajeta.nucleo.sparse`) are CSR/CSC/COO compressed storage for
  arrays that are mostly zeros, plus the handful of sparse linear-algebra operations
  that justify the type's existence (sparse matmul, sparse solve, conjugate gradient).
- **Extended linear algebra** (`dev.cajeta.nucleo.linalg`) is the matrix factorizations
  (`qr`/`cholesky`/`lu`/`svd`/`eig`/`eigh`) returned as typed records
  (`QrResult { q, r }`, `SvdResult { u, s, v }`, …) instead of the positional
  `Tensor[]` bags the stdlib computes today. The numerics already exist in
  `cajeta.math.linalg`. This surface gives them a typed, named, normalized return.

Both are native Cajeta over the `Tensor`/GEMM core, with no foreign BLAS/LAPACK (the
"cajeta owns its math" decision; analysis §3.4 *Drop*), and both keep
Fortran-order/LAPACK conventions out of the API surface.

### 1.2 Scope
- A sparse array core type with CSR, CSC, and COO storage formats, built
  over `Column<T>`/tensor buffers. The index and value buffers are columns.
- Construction from COO triplets (`rows`, `cols`, `vals`, `shape`) and conversion
  between formats.
- Sparse linear algebra: sparse × dense matmul, sparse × sparse matmul,
  `spsolve` (sparse direct solve), and `cg` (conjugate gradient, iterative).
- A typed-record return layer over `cajeta.math.linalg`'s factorizations:
  `qr`, `cholesky`, `lu`, `svd`, `eig`, `eigh`.
- Normalized, consistent return conventions across the whole surface, which means records
  rather than ad-hoc tuples.

### 1.3 Non-goals
- **A sparse *matrix* type.** SciPy is itself deprecating its `np.matrix`-based sparse
  matrix class, and we ship sparse arrays only (analysis §3.4 *Drop*). There is no
  `*`-means-matmul, no 2-D-always semantics, and no `.A`/`.H` matrix quirks. Sparse arrays
  follow the same n-D array conventions as the dense tensor.
- **A foreign BLAS/LAPACK / SuiteSparse dependency.** Numerics are native over the
  `Tensor`/GEMM core (§1.1). Whether a heavy decomposition may optionally delegate to
  a backend is a plan-time question (§10) and never a v1 dependency.
- **Fortran-order / column-major surface leakage.** Internal storage choices stay
  internal. The API is row-major n-D arrays like the rest of núcleo (analysis §3.4).
- **The rest of the SciPy algorithm modules.** `optimize`, `signal`, `interpolate`,
  `integrate`, `special`, `spatial`, `stats`, `ndimage`, and `cluster` are separate,
  independently-shippable specs. This spec is the sparse type plus the linalg returns.
- **Sparse tensors of rank > 2 with full sparse contraction.** v1 targets the 1-D/2-D
  cases SciPy's `sparse` covers (vectors and matrices). Higher-rank sparse contraction
  is deferred.

### 1.4 Relationship to existing constructs
- `cajeta.math.linalg` already exists and computes `solve`/`det`/`inv` (LU with
  partial pivoting) plus `qr`/`cholesky`/`lu`/`svd`/`eigh`, but the factorizations
  return positional `#Tensor<E>[]` bags (`qr` → `[Q, R]`, `lu` → `[P, L, U]`, etc.).
  This spec does not re-derive the numerics. It adds `dev.cajeta.nucleo.linalg` as a
  typed-record façade that calls into the stdlib and packages the result.
- Records (`records-spec.md`) are the return vehicle. `QrResult`, `LuResult`,
  `SvdResult`, `EigResult`, and `CholeskyResult` are records, accessed by name (`res.q`) and
  optionally destructured (`var (q, r) = linalg.qr(a)`, `target-experience.md` §5).
- `Column<T>` (`nucleo-column-spec.md`) is the buffer substrate. A sparse array's
  index buffers and value buffer are columns, and a non-null numeric value column is
  bit-identical to a tensor buffer, so dense↔sparse handoff is copy-cheap.
- `Tensor` / GEMM (`nucleo-expr-spec.md`, stdlib `cajeta.math`) is the dense
  substrate sparse matmul and `cg` multiply against, and the type the factorizations
  decompose.
- Templates are monomorphized. `SparseTensor<E>` and the factorization functions
  are real per-`E` instantiations over `E extends Floating`, matching the stdlib's
  `solve<E extends Floating>` shape.

> **TBD (plan-time):** [S1] Type name and shape, `SparseTensor<E>` (one type carrying a
> format tag) vs. distinct `CsrArray`/`CscArray`/`CooArray` types vs. a `SparseArray<E>`
> abstraction with format as a value. `target-experience.md` §5 writes `SparseTensor`.

## 2. Sparse array — the one new core type

A sparse array stores only its nonzero entries, in one of three layouts:
- **COO** (coordinate) is parallel `rows`, `cols`, `vals` buffers. It is the natural
  construction/ingest format, and it is order-free.
- **CSR** (compressed sparse row) is `indptr`, `indices`, `vals`. It gives fast row slicing and
  sparse×dense matmul.
- **CSC** (compressed sparse column) is `indptr`, `indices`, `vals`. It gives fast column slicing
  and is the transpose-friendly dual of CSR.

All three are arrays rather than matrices, with the same n-D array semantics as the dense tensor
(§1.3).

**Use cases**
- **2.1** As a developer, when I build a sparse array from COO triplets
  (`SparseTensor.fromCoo(rows, cols, vals, shape: [n, n])`), then I get a sparse array of
  the given shape whose nonzeros are exactly the supplied triplets, with the value type
  inferred from `vals` (`target-experience.md` §5).
- **2.2** As a developer, when I supply duplicate `(row, col)` coordinates at COO
  construction, then they are summed (the SciPy `coo` convention), deterministically. It is
  never last-write-wins.
- **2.3** As a developer, when I convert a sparse array between formats
  (`coo.toCsr()` / `csr.toCsc()` / `.toCoo()`), then the logical contents are identical
  and the conversion is a pure restructure of the index/value buffers.
- **2.4** As a developer, when I read the shape, nonzero count (`nnz`), and value dtype of
  a sparse array, then each is available without densifying. Metadata is O(1).
- **2.5** As a developer, when I densify a sparse array (`.toDense()`), then I get a
  `Tensor<E>` with zeros in the absent positions. The inverse, building a sparse
  array from a dense tensor, keeps only the nonzeros.
- **2.6** As a developer, when I use a sparse array where the API expects 2-D
  (matmul/solve), then it behaves as a matrix-shaped array. There is no separate
  "sparse matrix" type with `*`-means-matmul semantics (§1.3).

> **TBD (plan-time):** [S2] Default storage format. COO is the natural ingest format but a
> poor compute format. Whether `fromCoo` eagerly compresses to CSR, stays COO until an
> operation forces compression, or exposes the format explicitly is a plan-time choice.

## 3. Sparse storage over columns

A sparse array's three buffers (index, index, value, or indptr/indices/values) are
columns/tensor buffers, so the value buffer of a non-null numeric sparse array is
bit-identical to a dense tensor buffer (the column invariant, `nucleo-column-spec.md`
§1.1). This keeps sparse↔dense handoff and interop cheap.

**Use cases**
- **3.1** As a núcleo author, when I build a `SparseTensor<float32>`, then its `vals`
  buffer is a non-null numeric `Column<float32>` (== tensor bytes) and its index buffers
  are integer columns, with no bespoke allocation path.
- **3.2** As a núcleo author, when I hand a sparse array's value buffer to a dense kernel
  (e.g. the GEMM core for sparse×dense matmul), then it is the same byte layout a tensor
  would present, with no marshalling.

> **TBD (plan-time):** [S3] Index buffer width, 32-bit vs. 64-bit indices (and whether a
> large-index variant is needed for arrays exceeding 2³¹ entries), mirroring the column
> spec's large-offset question.

## 4. Sparse linear algebra — matmul

**Use cases**
- **4.1** As a developer, when I multiply a sparse array by a dense tensor
  (`sparse.matmul(a, x)` or `a @ x`), then I get a dense `Tensor<E>` result computed by
  iterating the sparse nonzeros against the dense operand, with no densification of `a`.
- **4.2** As a developer, when I multiply two sparse arrays
  (`sparse.matmul(a, b)` with both sparse), then I get a sparse result whose
  structure is the structural product of the two operands' sparsity patterns.
- **4.3** As a developer, when I attempt a matmul whose inner dimensions disagree, then it
  is rejected (a shape error), consistent with the dense `@` contract
  (`target-experience.md` §6).
- **4.4** As a developer, when the multiply is sparse×dense, then the result type is dense,
  and when it is sparse×sparse the result type is sparse. The return type follows the
  operands rather than a runtime flag.

> **TBD (plan-time):** [S4] Operator surface, whether sparse arrays participate in the
> `@` matmul operator (`target-experience.md` §6) directly, or only via `sparse.matmul`
> functions in v1. Resolved jointly with `nucleo-expr-spec.md`'s operator story.

## 5. Sparse linear algebra — solve and conjugate gradient

**Use cases**
- **5.1** As a developer, when I directly solve a sparse system
  (`sparse.spsolve(a, b)`) for a square sparse `a` and dense `b`, then I get the dense
  solution `x` with `a · x = b`, computed natively by a sparse-aware elimination over the
  `Tensor`/GEMM core with no foreign solver. It is the sparse analogue of stdlib `solve`.
- **5.2** As a developer, when I solve an SPD sparse system iteratively
  (`sparse.cg(a, b)`, conjugate gradient), then I get a dense approximate solution `x`,
  with optional `tol`/`maxiter`/`x0` named arguments controlling convergence
  (`target-experience.md` §5: `linalg.sparse.cg(laplacian, b)`).
- **5.3** As a developer, when `cg` does not converge within `maxiter`, then the outcome
  is reported explicitly, as a typed result carrying the iterate, residual, and a converged
  flag (normalized return, §7). It is never a silent bad answer.
- **5.4** As a developer, when I pass a `cg` operand that is not symmetric positive
  definite, then the contract is documented as undefined-for-non-SPD (CG's precondition),
  and a fail-loud diagnostic is preferred over silent divergence where detectable.
- **5.5** As a library author, when I express `cg`, then it is written against an
  abstract "apply A to a vector" (matvec) operation, so it works for any operand that
  provides a matvec and not only an explicit CSR array. That is a matrix-free path.

> **TBD (plan-time):** [S5] `cg` return shape, a bare `Tensor<E>` (SciPy-like, with the
> convergence info dropped) vs. a typed `CgResult { x, residual, iterations, converged }`
> record. The normalized-return ethos (§7, analysis §3.4 *Drop*) leans toward the record.
> `target-experience.md` §5 writes the bare-tensor call site. Reconcile.

> **TBD (plan-time):** [S6] Which sparse solvers land in v1. `cg` is the clear first
> iterative solver and `spsolve` the clear first direct one. Whether other Krylov methods
> (BiCGSTAB, GMRES) or a preconditioner interface are v1 or later is open.

## 6. Extended dense linear algebra — typed-record factorizations

`dev.cajeta.nucleo.linalg` wraps the stdlib `cajeta.math.linalg` factorizations and
returns typed records instead of positional `Tensor[]` bags. The numerics already
exist (`cajeta.math.linalg` computes `qr`→`[Q,R]`, `lu`→`[P,L,U]`, `svd`, `eigh`,
`cholesky` natively, no LAPACK). This surface is the typed, named, normalized return.

**Use cases**
- **6.1** As a developer, when I call `linalg.qr(a)`, then I get a `QrResult { Tensor q;
  Tensor r; }` and access `res.q` / `res.r` by name. A typo (`res.qr`) is a compile
  error. This replaces the positional `[Q, R]` bag (`target-experience.md` §5; records
  §5.2).
- **6.2** As a developer, when I destructure a factorization
  (`var (q, r) = linalg.qr(a)`), then the record's fields bind to locals in field order
  (records §5.3). The destructuring sugar is separable, since field access works without it.
- **6.3** As a developer, when I call `linalg.svd(a)`, then I get
  `SvdResult { Tensor u; Tensor s; Tensor v; }` with singular values `s` in descending
  order (the convention the stdlib `svd` already produces), accessed by name.
- **6.4** As a developer, when I call `linalg.cholesky(a)` on a symmetric positive
  definite `a`, then I get `CholeskyResult { Tensor l; }` (lower-triangular `L` with
  `L·Lᵀ = A`). A non-SPD input fails loud (the stdlib `cholesky` precondition). It never
  yields a NaN-laden `L`.
- **6.5** As a developer, when I call `linalg.lu(a)`, then I get
  `LuResult { Tensor p; Tensor l; Tensor u; }` (the permutation, lower, and upper
  factors), each by name, replacing the `[P, L, U]` bag.
- **6.6** As a developer, when I call `linalg.eigh(a)` on a symmetric `a`, then I get
  `EighResult { Tensor w; Tensor v; }` (ascending eigenvalues `w`, orthonormal
  eigenvector columns `V`), matching the stdlib `eigh` ordering.
- **6.7** As a developer, when I call `linalg.eig(a)` on a general (non-symmetric) `a`,
  then I get an `EigResult { Tensor w; Tensor v; }` of (possibly complex) eigenvalues and
  eigenvectors. General `eig` is the one factorization the stdlib lists as not
  yet implemented, so its v1 status is a question (§10).
- **6.8** As a developer, when I call the already-shipped `linalg.solve(a, b)` /
  `linalg.det(a)` / `linalg.inv(a)`, then they return their natural single values
  (`Tensor` / scalar / `Tensor`) unchanged. There is no record wrapping where there is one
  result, since records exist for multi-value returns (§7).

> **TBD (plan-time):** [L1] Whether `dev.cajeta.nucleo.linalg` re-exports the stdlib
> single-result functions (`solve`/`det`/`inv`) so callers import one module, or whether
> they stay in `cajeta.math.linalg` and only the factorizations are wrapped here.

> **TBD (plan-time):** [L2] Complex-number support for `eig`/`svd`. General `eig` produces
> complex eigenvalues. Whether the record fields are complex-typed tensors (requires a
> complex dtype in the substrate) or v1 restricts to the real/symmetric cases
> (`eigh`/`svd`) only is open.

## 7. Normalized return conventions

SciPy's returns are inconsistent, with ad-hoc tuples here and `OptimizeResult`/named bags
there (analysis §3.4 *Drop*). núcleo normalizes them. Every multi-value return is a typed
record, and every single-value return is the bare value.

**Use cases**
- **7.1** As a developer, when any núcleo linalg/sparse function returns more than one
  logical value, then it returns a named record (`QrResult`, `SvdResult`, `CgResult`,
  …). It never returns a positional tuple or an untyped array I index by position.
- **7.2** As a developer, when a function returns exactly one value, then it returns that
  value directly, with no single-field record ceremony, e.g. `solve`/`det`/`spsolve`.
- **7.3** As a developer, when an iterative method carries convergence metadata
  (`cg`), then that metadata is fields on the result record (residual, iterations,
  converged) rather than out-parameters or a separate status tuple.
- **7.4** As a developer, when I read the field names of any result record across the
  surface, then naming is consistent (`q`/`r`, `u`/`s`/`v`, `l`, `p`/`l`/`u`,
  `w`/`v`). The names match the mathematical letters rather than SciPy's varied keys.

## 8. Native-only numerics (no foreign BLAS/LAPACK)

Per the math decision (analysis §3.4 *Drop*, and the stdlib `LinAlg` doc-comment "no
foreign BLAS/LAPACK"), every operation in this spec is computed in native Cajeta over
the `Tensor`/GEMM core.

**Use cases**
- **8.1** As a núcleo author, when I implement a factorization or sparse op, then it is
  written against `cajeta.math.Tensor`/GEMM and the language's own numerics. There is no
  link against BLAS, LAPACK, SuiteSparse, or any foreign solver.
- **8.2** As a developer, when I read the API surface, then no Fortran-order, leading
  dimension, `lwork`, `jobz`, or other LAPACK-shaped parameter is exposed. The surface
  is row-major n-D arrays and named arguments (analysis §3.4 *Drop*).

> **TBD (plan-time):** [L3] Native-vs-optional-backend for heavy decompositions
> (`svd`/`eig` on large matrices, sparse direct factorization). v1 is native-only by the
> math decision. Whether a later optional backend seam (behind the same record API,
> off by default, no link in the default build) is worth designing for now is open, so that
> the API doesn't have to change if performance later demands it.

## 9. Acceptance criteria (spec-level)
- A sparse array can be constructed from COO triplets (with duplicate-summing), converted
  among COO/CSR/CSC losslessly, and densified to / built from a `Tensor`.
- Only a sparse array type exists. There is no deprecated sparse matrix type and no
  `*`-means-matmul semantics.
- A sparse array's value buffer is a non-null numeric column (== tensor bytes), and its index
  buffers are integer columns.
- Sparse×dense matmul returns a dense tensor, and sparse×sparse returns a sparse array.
  Inner-dimension mismatch is rejected.
- `spsolve` solves a square sparse system natively. `cg` solves an SPD system iteratively
  with named `tol`/`maxiter`/`x0` controls and an explicit non-convergence outcome.
- `qr`/`cholesky`/`lu`/`svd`/`eigh` (and `eig` per its v1 status) return typed records
  with named, mathematically-conventional fields and no positional `Tensor[]` bags, and
  single-result functions return bare values.
- All numerics are native over the `Tensor`/GEMM core, with no foreign BLAS/LAPACK link and no
  Fortran-order/LAPACK parameters on the surface.

## 10. Open questions (resolve at plan time)
- **[S1]** Sparse type name/shape, one `SparseTensor<E>` with a format tag vs. distinct
  per-format types vs. a `SparseArray<E>` with format as a value (§1.4).
- **[S2]** Default storage format and when COO compresses to CSR (eager / lazy /
  explicit) (§2).
- **[S3]** Index buffer width (32- vs. 64-bit; large-index variant) (§3).
- **[S4]** Whether sparse arrays use the `@` matmul operator or only `sparse.matmul` in
  v1 (§4), joint with `nucleo-expr-spec.md`.
- **[S5]** `cg` return, bare `Tensor` vs. a `CgResult` convergence record (§5);
  reconcile with the `target-experience.md` §5 call site.
- **[S6]** Which sparse solvers in v1, `cg`+`spsolve` only, or also BiCGSTAB/GMRES and a
  preconditioner interface (§5).
- **[L1]** Whether `dev.cajeta.nucleo.linalg` re-exports the stdlib single-result functions
  (`solve`/`det`/`inv`) or only wraps the factorizations (§6).
- **[L2]** Complex-number support for `eig`/`svd`, complex-typed record fields vs. v1
  real/symmetric only (§6).
- **[L3]** Native-vs-optional-backend for heavy decompositions. v1 is native-only; whether
  to design an optional off-by-default backend seam behind the same record API now (§8).
