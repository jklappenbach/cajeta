# Dense Linear-Algebra Solvers — Specification

> Status: draft for review (2026-07-29). Completes **`cajeta.math.linalg`** from
> square-only foundation grade to consumer grade — the solver layer downstream
> libraries stand on (first consumer: an external `cajeta-ml` linear-regression
> library; layering per `python-stack-analysis.md` §4.7 — the external
> `dev.cajeta.scipy` façade skins these numerics, and the sparse array type is
> specced separately in `nucleo-sparse-linalg-spec.md` as re-scoped 2026-07-29).
> Backend stays **native cajeta over the Tensor/GEMM core — no foreign
> BLAS/LAPACK** (the standing "cajeta owns its math" decision, numpy-porting
> Phase 11). Outline-numbered for addressability. Plan-time decisions are
> marked `[D*]` and collected in §9.

## 1. Definition

### 1.1 Purpose
`cajeta.math.linalg.LinAlg` already ships the numpy.linalg core — `solve`,
`det`, `inv`, `cholesky`, `lu`, `qr`, `eigh`, `svd`, `pinv`, `lstsq`,
`matrixRank`, `cond`, `normFro`, `eigvals` — and `Tensor` carries the product
layer (`matmul`, `matmulBatched`, `dot`, `vdot`, `inner`, `outer`, `kron`,
`tensordot`, `einsum`). What is missing is not coverage but **shape and
algorithm generality**: every LinAlg entry point assumes a **square** `(n,n)`
matrix and a **single** right-hand-side vector, `qr` is modified-Gram-Schmidt
(loses orthogonality on ill-conditioned input), `svd` goes through the Gram
matrix `AᵀA` (squares the condition number), and `lstsq` rides `pinv` — the
slowest, least accurate least-squares route, and square-only. There is **no
triangular solve**, so a factorization cannot be *applied*: every solve
re-factors.

This spec closes those gaps: rectangular factorizations, multi-RHS solves,
triangular/factor-application solvers, consumer-grade `lstsq`, `slogdet`, and
a norms surface.

### 1.2 Current state (what exists / what's wrong with it)

| Entry point | Today | Gap |
|---|---|---|
| `solve(A, b)` | Gaussian elim + partial pivot, `(n,n)`×`(n,)` | no multi-RHS `(n,k)` |
| `cholesky(A)` | lower factor, SPD `(n,n)` | no way to *apply* the factor (no triangular solve) |
| `lu(A)` | `[P,L,U]` bag | same — no `luSolve` |
| `qr(A)` | MGS, square only | rectangular missing; MGS orthogonality degrades with `cond(A)` |
| `svd(A)` | via `eigh(AᵀA)`, square only | Gram route halves accurate digits on ill-conditioned input; rectangular missing |
| `pinv(A)` | via svd, square only | rectangular missing |
| `lstsq(A, b)` | `x = pinv(A)·b`, square only | **the regression entry point cannot take a design matrix** `(m,n)`, `m≠n` |
| `matrixRank(A)` | tol = `σmax·1e-4` | ad-hoc tolerance; numpy uses `σmax·max(m,n)·ε` |
| `cond(A)` | σmax/σmin, square | rectangular missing |
| `normFro(A)` | square only | rectangular missing; no vector norms, no 1/∞ matrix norms |
| `det(A)` | product of pivots | overflows/underflows on large `n` — no `slogdet` |
| `eigvals(A)` | general real, values only, f64 only | vectors deferred (§1.4) |

### 1.3 Consumers driving the requirements
- **`cajeta-ml` linear regression** (the commissioning consumer): tall design
  matrices `(m samples, n features)`, QR-based `lstsq`, normal-equations path
  (`cholesky` + `choSolve`), multi-output targets (multi-RHS), standard
  errors (`choSolve` against `I`), `cond` for diagnostics.
- **Ridge/λ-path, Kalman-class filters, Gaussian processes**: factor once,
  solve many — triangular solves, `choSolve`, `slogdet` for log-likelihoods.
- **PCA / dimensionality reduction**: rectangular `svd` with true (not
  Gram-degraded) accuracy.
- **Sparse solvers** (external, per python-stack-analysis §4.7): iterative
  methods (`cg`-family) need vector norms for convergence checks.

### 1.4 Non-goals
- **Complex dtype / general `eig` with eigenvectors.** `eigvals` (Francis QR,
  values-only) stays as is; non-symmetric eigenvectors need a complex tier or
  inverse iteration and have no commissioning consumer. Recorded as future.
- **Batched/broadcast factorizations** (`solve` over `(b,n,n)` stacks) —
  future; `matmulBatched` is precedent when a consumer arrives.
- **GPU linalg.** CPU floor, like the existing unit. The GEMM-heavy pieces
  (Gram products, blocked updates) already route through `Tensor.matmul`.
- **Foreign BLAS/LAPACK.** Standing decision.
- **Typed-record returns.** Stdlib keeps positional bags; records ride the
  external `dev.cajeta.scipy` façade (python-stack-analysis §4.7).
- **Sparse.** Separate drafted spec.
- **Autograd VJP rules for linalg ops** (differentiable `solve`/`cholesky`) —
  future, with the nucleo-autograd registry.

## 2. Feature: triangular + factor-application solvers

The missing primitive: solving against an already-computed factor.

- `solveTriangular(T, B, lower, unitDiag, transposed)` — forward/back
  substitution, `(n,n)` triangular × `(n,)` or `(n,k)`; `transposed` solves
  `Tᵀx=b` without materializing the transpose.
- `choSolve(L, B)` — SPD solve from a `cholesky` factor: two triangular
  solves, half the cost of LU and no re-factor.
- `luSolve(P, L, U, B)` — solve from a `lu` bag.

Use cases:
- 2.1 Ridge regression λ-path: factor `AᵀA + λI` once per λ — or factor once
  and re-solve for many RHS — instead of full `solve` each time.
- 2.2 Kalman update: `choSolve` against the innovation covariance factor for
  gain and likelihood in one factorization.
- 2.3 GP posterior: `choSolve(L, K*)` for mean and variance from one factor.
- 2.4 Standard errors: `(AᵀA)⁻¹` diagonal via `choSolve(L, I)`.

## 3. Feature: multi-RHS square solve

`solve(A, B)` with `B` `(n,k)`, alongside the existing vector overload. One
elimination, `k` back-substitutions.

Use cases:
- 3.1 Multi-output regression (k targets, one factorization).
- 3.2 `inv` becomes `solve(A, I)` internally — one code path.

## 4. Feature: rectangular factorization core

- `qr(A)` on **Householder reflections** for `(m,n)`, `m≥n` and `m<n`, with
  reduced (`Q (m,min)`, `R (min,n)`) and full modes `[D1]`. Replaces MGS
  internally; the square `[Q,R]` bag contract is unchanged.
- `svd(A)` on **Golub–Kahan bidiagonalization + implicit-shift QR** for
  `(m,n)` — no Gram matrix, full accuracy on ill-conditioned input. Square
  results match the current contract (`[U, S, Vt]`, S descending).
- `pinv(A)`, `normFro(A)`, `cond(A)`, `matrixRank(A)` generalize to `(m,n)`
  on top.

Use cases:
- 4.1 Design-matrix QR: the numerically-correct regression path.
- 4.2 PCA on `(samples, features)` without forming the covariance matrix.
- 4.3 Minimum-norm solutions for underdetermined systems (`m<n`).
- 4.4 Ill-conditioned input (`cond ≈ 1e6` at float32): Gram-based svd loses
  half the digits; bidiagonal svd does not.

## 5. Feature: consumer-grade `lstsq`

`lstsq(A, B)` re-founded: `(m,n)` × `(m,)` or `(m,k)`. Full-rank fast path
via Householder QR (`Rx = Qᵀb` + back-substitution); rank-deficient fallback
via svd (minimum-norm, numpy semantics). Return stays `x` (`(n,)`/`(n,k)`)
`[D2]`.

Use cases:
- 5.1 `cajeta-ml` `LinearRegression.fit` — the commissioning call.
- 5.2 Polynomial/basis fitting (`cajeta.math.poly` upgrade path).
- 5.3 Overdetermined calibration problems (sensor fitting, curve fitting).

## 6. Feature: scalar diagnostics

- `slogdet(A)` → `(sign, log|det|)` — determinants that neither overflow nor
  underflow.
- `matrixRank` tolerance moves to numpy semantics: `σmax · max(m,n) · ε(E)`.
- `cond` on rectangular input (σmax/σmin of the thin svd).

Use cases:
- 6.1 GP / multivariate-normal log-likelihood (`log det` of a covariance).
- 6.2 Model selection (AIC/BIC) without `det` overflow at `n ≳ 100`.
- 6.3 Reliable rank/conditioning warnings in `cajeta-ml` diagnostics.

## 7. Feature: norms

- Vector: 1, 2, ∞, and general-p norms (`normVec(x, p)` family).
- Matrix: Frobenius on `(m,n)`; induced 1-norm (max abs column sum) and
  ∞-norm (max abs row sum).

Use cases:
- 7.1 Iterative-solver convergence checks (sparse spec's `cg` residual).
- 7.2 Regularization terms (L1/L2) in `cajeta-ml`.
- 7.3 Error metrics (`‖Ax−b‖` reporting in `lstsq` consumers).

## 8. Conventions and accuracy contract

- 8.1 Element types: `E extends Floating` (float32 + float64), like today.
  `eigvals` stays f64-only.
- 8.2 Row-major `Tensor<E>` surface; no Fortran-order leakage (sparse spec
  §1.3 rule).
- 8.3 Inputs are never modified (copy-in, like the existing unit).
- 8.4 Accuracy is pinned against numpy/scipy-computed fixtures in
  `test/math/` (the NumpyOpsTests §11b convention): float32 tolerance ~1e-3
  absolute on well-conditioned inputs, plus dedicated ill-conditioned cases
  asserting the qualitative bounds (orthogonality `‖QᵀQ−I‖`, reconstruction
  `‖QR−A‖`, Moore-Penrose identities).
- 8.5 Existing call sites keep working: `qr`/`svd`/`lstsq`/`solve` signatures
  and bag shapes are preserved on square input; new behavior is additive
  (rectangular shapes, new overloads, new functions).

## 9. Plan-time decisions

- **[D1] QR mode surface** — reduced-only (numpy default) vs a `full` flag.
  Lean: reduced only in v1; full mode has no listed consumer.
- **[D2] `lstsq` extras** (residuals, rank, singular values) — bag them now
  vs leave for a façade record. Lean: return `x` only; rich returns are the
  external scipy façade's job (python-stack-analysis §4.7).
- **[D3] Where the new tests live** — extend `NumpyOpsTests.cpp` (already
  ~2900 lines) vs a new `test/math/LinAlgSolversTests.cpp`. Lean: new file.
- **[D4] `solveTriangular` flag spelling** — three booleans vs an options
  int/enum. Lean: booleans, matching scipy's `solve_triangular` semantics.
