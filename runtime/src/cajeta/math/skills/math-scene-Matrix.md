---
id: math-scene-Matrix
applies-to: [cajeta/math/Matrix]
title: Matrix<T,R,C> — fixed-shape intrinsic matrix value type
description: Using cajeta.math.Matrix<T,R,C> — the Pythonic operator surface (a+b, a*b=matmul, m[r][c]) over a compiler-lowered flat row-major vector; construction, ops, methods, and the zero-field/interception sharp edges.
---

# Matrix&lt;T, R, C&gt;

Fixed-shape numeric matrix in `cajeta.math`: `R` rows × `C` columns of element type
`T`, a **value type** that lowers to a flat row-major `<R*C x T>` (element `(r,c)` at
lane `r*C+c`) and is passed by value with **no heap allocation**. Reach for it for
small dense linear algebra (transforms, solves, masks); the *same* representation and
operators run on CPU, Vulkan, and AMD device kernels. For a 1-D run of lanes use
`Vector<T,N>` (its `dot`/`length`/`normalize` and `.x/.y/.z/.w` complement this type).

This is a **value type you construct directly** with `stack` — not an access point you
receive from a factory, and not something you `heap`-allocate (though
`KernelBuffer<Matrix<...>>` stores them by value).

## The hybrid: declared surface vs. what actually runs

`Matrix.cajeta` declares only the **Pythonic operator surface** (`+ - * /`, `[]`,
`row/col/transpose/...`). The method/operator **bodies are resolution placeholders and
are never emitted** — every use site is **intercepted by codegen**, which computes the
result *shape* and lowers the op to register-native vector intrinsics. Consequences you
must code around:

- The declared signatures **lie** where the real result is shape-dependent. `operator*`
  is declared `(Matrix, T)` (scalar scale) and `operator==` is declared to return
  `boolean`, but at the use site `*` is **matrix multiply** and `==` yields a **per-lane
  mask** `Matrix<boolean,R,C>`. Trust this skill / the sample, not the signature.
- The class carries a single `T reserved;` field purely to give a zero-field class a
  valid LLVM layout. It is **not data** — never read or write `m.reserved`.
- Matrix*Matrix, Matrix*Vector, `determinant`, and `inverse` are K-/shape-generic and so
  exist **only** as interceptions — there is no callable method body and no real
  dispatch (deferred; see the B1 plan).

## Construct

```cajeta
// Matrix and Vector are intrinsic — no import needed for the types themselves.
Matrix<float32,2,2> a = stack Matrix<float32,2,2>(1.0f, 2.0f, 3.0f, 4.0f); // [1 2; 3 4]
Matrix<float32,2,3> m = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f, 4.0f,5.0f,6.0f);
```

Pass **exactly `R*C` element args in flat row-major order**. Wrong count is the
compile-time diagnostic `CAJETA_ERROR_MATRIX_CONSTRUCT`; a zero dimension
(`Matrix<float32,0,2>`) is `CAJETA_ERROR_MATRIX_DIMENSIONS`.

## Operators (use-site semantics)

- `a + b`, `a - b`, `a / b` — **element-wise**, operands must be the **same shape**
  (else `CAJETA_ERROR_MATRIX_SHAPE`).
- `a * b` — **MATRIX MULTIPLY**: `Matrix<T,R,K> * Matrix<T,K,C> -> Matrix<T,R,C>`
  (inner `K` checked → `CAJETA_ERROR_MATRIX_SHAPE`); `Matrix<T,R,C> * Vector<T,C> ->
  Vector<T,R>`; `m * scalar` / `scalar * m` → element-wise scale. **Element-wise product
  is `hadamard(b)`, never `*`.**
- `m[r]` → `Vector<T,C>` (row r); `m[r][c]` → element; `m[r][c] = v` assigns. Dynamic
  (runtime `int32`) indices work.
- `== != < <= > >=` → a **per-lane mask** `Matrix<boolean,R,C>` (value-type comparison
  rule, *not* a reduced boolean). A scalar RHS broadcasts. Reduce with `.all()` /
  `.any()`; blend with `mask.select(a, b)`.

```cajeta
Matrix<float32,2,2> b = stack Matrix<float32,2,2>(5.0f, 6.0f, 7.0f, 8.0f);
Matrix<float32,2,2> c = a * b;                 // matmul -> [19 22; 43 50]
boolean same = (a == a).all();                 // whole-matrix equality -> true
Matrix<float32,2,2> z = stack Matrix<float32,2,2>(0.0f,0.0f,0.0f,0.0f);
Matrix<float32,2,2> kept = (a > 1.0f).select(a, z);  // per-lane blend on broadcast scalar
```

## Methods that matter

- `transpose()` → `Matrix<T,C,R>` (element `(i,j)`→`(j,i)`).
- `row(i)` → `Vector<T,C>`; `col(j)` → `Vector<T,R>`.
- `hadamard(b)` → `Matrix<T,R,C>` element-wise product (same shape).
- `identity()` → `Matrix<T,N,N>` — **square only**.
- `determinant()` → scalar `T` — square **2×2 / 3×3 / 4×4** only.
- `inverse()` → `Matrix<T,N,N>` — **square only**; solve `A·x = b` via `A.inverse() * b`.

`identity`/`inverse`/`determinant` on a **non-square** shape, and any **unknown
method**, are the compile-time diagnostic `CAJETA_ERROR_MATRIX_METHOD`.

```cajeta
Matrix<float32,2,2> g  = stack Matrix<float32,2,2>(4.0f, 7.0f, 2.0f, 6.0f);
Matrix<float32,2,2> gi = g.inverse();          // det = 10
Vector<float32,2>   rhs = stack Vector<float32,2>(18.0f, 14.0f);
Vector<float32,2>   x   = gi * rhs;            // solves g*x = rhs -> (1, 2)
```

## Ownership, lifecycle & concurrency

It is a **by-value** type: assignment/return/parameter passing **copies the lanes**,
there is no aliasing and **nothing to free** — no `#` transfer, no `close()`/dispose, no
drop chain. Storing in `KernelBuffer<Matrix<...>>` likewise copies by value. Element type
`T` is itself a scalar (e.g. `float32`, `int32`, `boolean`); a `Matrix` over a class `T`
is not the intended use.

## What it does NOT do

- **No dynamic resizing / runtime shape.** `R` and `C` are compile-time `uint32` type
  params; all shape checks are compile-time diagnostics, not runtime exceptions.
- **`*` is not element-wise** — use `hadamard(b)`. **`==` is not a boolean** — it is a
  mask; collapse it with `.all()`/`.any()`.
- **No `determinant`/`inverse` beyond 4×4 square**, and none on non-square.
- **No real method dispatch** — bodies are intercepted placeholders; do not expect to
  call, override, or take the address of these operators/methods, and do not touch the
  `reserved` field.
