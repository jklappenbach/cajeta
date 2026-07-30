---
id: math-overview
applies-to: [cajeta.math]
title: cajeta.math — Tensor (numpy port) + cajeta-gfx value types
description: Routing map for cajeta.math — pick the Tensor/ndarray system vs the gfx linear-algebra/geometry value types, with the package-wide ownership rules.
---

# cajeta.math — orientation & routing

`cajeta.math` is **two unrelated domains under one package**. Decide which you are
in *first* — the rules below differ between them:

1. **Tensor / ndarray** — the numpy port. Runtime-rank, dtype-in-`T`, heap-backed,
   **refcounted/RAII**. Use for n-D numeric arrays, broadcasting, reductions, ML.
2. **cajeta-gfx value types** — fixed-shape linear algebra & geometry (`Matrix`,
   `Color`, `Transform`, `Ray`, `Aabb`, …). **Stack-by-default value types**, no
   heap, no drop chain. Use for renderer/scene-graph/3D math.

## Task → entry point

| You want to… | Start with | Domain |
|---|---|---|
| An n-D array (zeros/ones/full/arange/from data) | `Tensor.zeros<E>` / `ones` / `full` / `arange` / `of` (method-templated statics) | Tensor |
| Reshape / transpose / slice / squeeze / broadcast a tensor | `Tensor` view methods (`reshape`, `transpose`, `slice`, `broadcastTo`, …) | Tensor |
| Index a tensor (basic / boolean / fancy) | `Tensor.index` / `maskedSelect` / `take` | Tensor |
| The dtype of a static type / promote two dtypes | `DType.of<T>()`, `DType.promote(a,b)` (NEP-50) | Tensor |
| float→int with an explicit rounding policy | `Cast.roundToInt<I>(x, RoundingMode.…)` | Tensor |
| Zero-copy hand a tensor to/from an external lib | `Tensor.protocol()` ↔ `Tensor.fromProtocol(p)` | Tensor |
| Move a tensor to/from GPU | `Tensor.gpu()` / `cpu()` / `isOnGpu()` | Tensor |
| Solve `A·x=B` (square, multi-RHS) / apply a factor | `LinAlg.solve`, `solveTriangular`, `choSolve`, `luSolve` (`cajeta.math.linalg`) | Tensor |
| Factor a matrix (rectangular OK) | `LinAlg.qr` (Householder, reduced), `svd` (Golub–Kahan bidiagonal), `lu`, `cholesky`, `eigh` | Tensor |
| Least squares / regression fit | `LinAlg.lstsq` (QR fast path, svd min-norm fallback; multi-RHS) | Tensor |
| Determinant / rank / conditioning / norms | `LinAlg.det`, `slogdet` (log-space, no overflow), `matrixRank` (numpy ε-tol), `cond`, `normFro`/`norm1`/`normInf`, `normVec`/`normInfVec` | Tensor |
| A small fixed matrix with `+ - * [r][c]`, `transpose`, `inverse` | `Matrix<T,R,C>` | gfx |
| Build a rotation (axis/angle, Euler) | `Rotation.fromAxisAngle` / `fromEuler` → `Quaternion<float32>` | gfx |
| TRS placement / model transform | `Transform` (stack) | gfx |
| Projection / view matrices | `Camera.perspective` / `ortho` / `lookAt` | gfx |
| Bounds, picking, culling | `Aabb`, `Sphere`, `Ray`, `Plane`, `Frustum` (all stack) | gfx |
| sRGB ↔ linear color | `Color.toLinear()` / `toSrgb()` | gfx |

**Not here (dead-end avoidance):**
- **`Vector<T,N>` and `Quaternion<T>` are compiler builtins, not classes in this
  package.** You don't import them; just write `Vector<float32,3>` /
  `Quaternion<float32>`. The gfx types are *built on* these builtins. (`Matrix` *is*
  declared here, but as an intercepted hybrid — see hazards.)
- Scalar math (`max`/`min`/`clamp`/`sin`/`tan`/`sqrt`) is **`cajeta.lang.Math`**,
  not this package.
- No 0-D scalar tensor in v1 (a 1-D index yields a length-1 1-D view).
- `complex64/128` dtypes are **reserved, not implemented**; the fp8/fp6/fp4 and
  128-bit dtypes exist but `Tensor.fromProtocol` returns `null` for them.
- Stochastic rounding is deferred (`RoundingMode.STOCHASTIC` → falls back to
  `NEAREST_EVEN`).

## Cross-cutting invariants

- **Lazy parse.** The whole package is parsed **on demand** — a program that never
  imports `cajeta.math` pays nothing at compile time. Any `import cajeta.math.X`
  (or a reference to an intercepted builtin type like `Matrix`) triggers the parse;
  `MathInfo` is the neutral anchor type that proves the generic on-demand path.
- **Two memory models, by domain:**
  - *gfx value types* are `@ValueType` / `final`: **`stack`-allocated by default,
    copied by value, no heap, no drop chain.** Construct with `stack Foo(...)`.
  - *Tensor* is **the only refcounted/RAII type.** `heap Storage<T>` is auto-dropped
    and the global live-set frees the one shared buffer **exactly once**, even when
    several `Tensor` views share it. Factories return an **owned `#Tensor<T>`**.
- **Ownership markers.** `#` on a return = ownership transferred to the caller
  (e.g. `#DType`, `#int64[]`, `#Tensor<E>`). A `Storage`/`Tensor` passed as a plain
  param is **borrowed** (the source keeps ownership); the factories `#`-move their
  `Storage` in. Views (`alias`, `reshape`-when-contiguous, `slice`, `transpose`,
  `broadcastTo`, `index`, …) **share** the backing `Storage` (no copy); `copy()`,
  `materialize`, `maskedSelect`, `take`, and a non-contiguous `reshape` make an
  **independent** buffer.
- **Errors.** Shape mismatch throws `BroadcastException` (a
  `RecoverableException` — catch it). `Tensor.fromProtocol` signals "unsupported
  dtype" by returning **`null`**, not throwing.
- **Builds.** A no-GPU build still works: `Storage`'s device mirror is backed by the
  `cajeta.xpu` CPU backend; `gpu()`/`cpu()` are eager (explicit), v1 policy.

## Canonical end-to-end example

```cajeta
import cajeta.math.Tensor;

// 2x3 float tensor, C-order; shapes are built as int64[] (heap + assign).
int64[] shp = heap int64[2];
shp[0] = 2;
shp[1] = 3;
Tensor<float32> a = Tensor.zeros<float32>(shp);   // owned #Tensor<float32>
a.set2(0, 1, 4.5f);                                // 2-D write
Tensor<float32> v = a.transpose();                 // 3x2 VIEW — shares a's Storage
// a and v drop at scope exit; the shared buffer frees exactly once (live-set).
```

gfx side (stack value types, builtins not imported):

```cajeta
import cajeta.math.Camera;
import cajeta.math.Rotation;

Matrix<float32,4,4> proj = Camera.perspective(1.0472f, 1.7778f, 0.1f, 1000.0f);
Vector<float32,3> up = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);
Quaternion<float32> yaw = Rotation.fromAxisAngle(up, 0.7853982f);
```

## Hazards

- **`cajeta.math.linalg` throws `LinAlgException`** (a `RecoverableException`) on
  shape violations and singular input (zero triangular diagonal); catch it rather
  than pre-validating shapes. `solve`/`solveTriangular`/`lstsq` accept `(n,)` or
  `(n,k)` RHS by rank-dispatch — there is no separate multi-RHS overload.

- **Compound `Vector`/`Matrix`/`Quaternion` expressions crash host codegen today.**
  Write **one operator (or one method call) per statement**, binding each
  intermediate to a local — e.g. `Vector v = dir * t; return origin + v;`, never
  `origin + dir * t`. A method-call result used *inline* as an operator operand has
  the same problem: pull it into a local first. Every gfx type's source follows
  this; copy that style.
- **`Matrix` is an intercepted hybrid, not an ordinary class.** Its method/operator
  bodies are resolution placeholders folded by codegen; `a * b` is **matrix
  multiply** (Hadamard is `a.hadamard(b)`), and a comparison yields a per-lane
  `Matrix<boolean,R,C>` mask (whole-matrix equality is `(a==b).all()`).
- **`DType` factory names are short** (`i32`, `f32`, `u8`, `bf16`, …) because the
  full primitive names (`int32`, `float32`, …) are reserved keywords and can't name
  a method. Use `DType.of<float32>()` to bridge from a static type.
- **Never bind a borrowed `Storage` to a named class-typed local** before re-wrapping
  it as a view — that registers an owning drop and frees the shared buffer out from
  under the producer (use-after-free). Pass the borrow inline (this is why
  `Tensor.rebuildShared` casts `p.base()` directly into the constructor).

## Downward pointers

Class-level depth lives in the per-type skills (when present) and the doc-comments
in each source file under `runtime/src/cajeta/math/`. Specs:
`documents/math/tensor-spec.md` (Tensor) and the cajeta-gfx plan (gfx types).
