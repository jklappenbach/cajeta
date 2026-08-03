# Host codegen SIGSEGV — matrix element as call argument — FIXED 2026-08-03

Origin: docs-refactor 15.8 (unit-12 GeometryDemo, 2026-07-03). Sibling of
the cajeta-gfx §1 "compound Vector expression" finding.

## 0. Resolution (2026-08-03)

**FIXED — one guard, and the §1 suspicion was right.** The defect is
argument-position lowering generally, exactly as §1's 2026-07-31 extension
concluded, and the three failure signatures (runtime SIGSEGV, isel `Cannot
select`, physreg-copy abort) are one bug wearing three hats.

**Root cause: a DOUBLE LOAD.** Element access lowers per container. A plain
array hands back the element GEP — a POINTER the caller must load — while
Matrix and Vector extract the lane and materialize it, handing back the
VALUE. The 2026-08-01 fix that made `f(arr[i])` work applies `loadIfLValue`
to every `ArrayIndexExpression` argument, which is right for the GEP shape and
wrong for the materialized one. The already-loaded Matrix element got loaded a
second time:

```llvm
%5 = load float, ptr %vec.idx.slot   ; the element's value
%6 = load float, float %5            ; "Load operand must be a pointer"
```

That is why the failure mode varied by source while the position never did —
the same double load is a verify error here, an isel failure there, and a
register-allocator abort somewhere else, depending on the type involved.

**Fix.** `loadIfLValue` returns immediately when its operand is not a pointer.
That is a universal invariant, not a Matrix special case: every branch of that
function emits a load, and a load from a non-pointer is unrepresentable. So
the guard covers Matrix `m[r][c]`, plain arrays, and `ArrayList.operator[]`
at once, and any future container that materializes its elements.

**Coverage against §2.1's use cases:**
- Use case 1 (`f(m[r][c])` correct, no ritual) — `MatrixTests.elementAsCallArgument`.
- Nested composition `f(g(m[1][1]))` — `MatrixTests.elementAsNestedCallArgument`.
- The extract-to-local control still works — `elementViaLocalAsCallArgument`.
- The plain-array sibling stays fixed — `plainArrayElementAsCallArgument`.
- Use case 2 (remove the tour workaround) — the `ArrayList.operator[]` hoist
  in `BPlusTreeDemo` is reverted to `asks.get(prices[k])`.

**Still owed:** `dev.cajeta.ml`'s `KNeighborsRegressor` carries a hoist naming
this spec; it can be reverted when ml is rebuilt for 0.15.0.

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

**2026-07-31 extension 2: ArrayList elements too (14-line repro).** On the
0.12.0 toolchain, `m.get(ks[k])` — an `ArrayList<int32>` `operator[]`
result passed directly as the argument of a `BPlusTree<int32,int32>.get`
call inside a while loop — aborts compile-time with `LLVM ERROR: Cannot
emit physreg copy instruction` (SIGABRT, exit 134). Found by tour-quality
unit 3 (BPlusTreeDemo realism rewrite; the identical shape with the element
in a COMPARISON — `ks[k] <= ks[k-1]` — compiles fine, as does the same
element-as-arg via plain `String[]`/`int32[]`/`float32[]` in
HashMapDemo/StatsDemo). Same extract-to-local workaround applied with a
comment naming this spec. So the affected argument-position sources now
span Matrix `m[r][c]`, plain arrays, and `ArrayList.operator[]`; the
failure mode varies (runtime SIGSEGV, isel `Cannot select`, physreg-copy
abort) but the position is always argument.

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
