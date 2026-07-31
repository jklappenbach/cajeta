# Host codegen SIGSEGV — matrix element as call argument — spec (draft)

Origin: docs-refactor 15.8 (unit-12 GeometryDemo, 2026-07-03). Sibling of
the cajeta-gfx §1 "compound Vector expression" finding.

## 1. Definition

Passing a `Matrix<float32,4,4>` element directly as a call argument —
`f(m[1][1])` — SIGSEGVs the built binary. Extracting to a local first
(`float32 x = m[1][1]; f(x)`) works, and direct `m[r][c]` inside `if`
comparisons is fine — which is why `GfxCameraTests` never caught it. The
workaround comment lives in tour GeometryDemo section 3.

**2026-07-31 extension: PLAIN ARRAY elements too.** `Math.sqrt(d2[s])` and
`tensor.get1(idx[s])` with `float64[] d2` / `int64[] idx` locals produced a
COMPILE-TIME LLVM isel abort (`Cannot select: load<(load (s64)…), anyext
from i64>` into f64) in `dev.cajeta.ml.KNeighborsRegressor::predict` on the
v0.12.1 toolchain — same argument-position element access, fixed by the
same extract-to-local ritual (applied there with a comment naming this
defect). The defect is therefore element-access-in-argument-position
lowering generally, not Matrix-specific; 2.1's soundness requirement
covers `f(arr[i])` for ordinary arrays as well.

Suspected shape: the host-side lowering of a value-type element access in
ARGUMENT position takes the address of a transient (or mis-sizes the load)
where statement/condition positions materialize correctly.

## 2. Features

### 2.1 Element-access arguments are sound
`f(m[r][c])`, `f(v[i])` (Vector), and nested compositions (`f(g(m[0][0]))`)
lower to correct loads on the host path.

Use cases:
1. As a gfx developer, when I pass matrix/vector elements straight into
   math calls, then results are correct and no crash occurs — no
   extract-to-local ritual.
2. As the tour's GeometryDemo, when the section-3 workaround is removed,
   then the demo still passes its self-checks.
3. As a test author, when `GfxCameraTests` gains an element-as-argument
   case, then the regression is pinned where the original gap hid.

### 2.2 Root-cause note (to fill during plan recon)
The plan's first unit is a minimal-repro dissection: single-subscript
Vector vs double-subscript Matrix, argument vs condition position, direct
call vs chained. Fix lands where the dissection points (likely the
argument-coercion path in MethodCallExpression / value-type element GEP).

## 3. Non-goals
Device (XPU) codegen — the finding is host-side; SIMD Vector ops
(separate plans).
