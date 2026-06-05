# B1 — `Matrix<T, R, C>` as a hybrid operator-overloaded value type

Linear-algebra value types, the first real exercise of the operator-overloading
surface for the standard library. `Matrix<T, R, C>` is a **hybrid**: a declared
`cajeta.math.Matrix` class supplies the Pythonic operator surface, while a
concrete `Matrix<...>` reference resolves to a flat row-major `<R*C x T>`
representation (`CajetaMatrix`) that codegen intercepts — identical generated
code to a pure-intrinsic value type, like `Vector<T,N>`.

## Locked design (owner)
- **`*` = matrix multiply.** `Matrix<T,R,K> * Matrix<T,K,C>` → `Matrix<T,R,C>`
  (inner dim K checked); `Matrix<T,R,C> * Vector<T,C>` → `Vector<T,R>`;
  `Matrix * scalar` → element-wise scale. Hadamard (element-wise product) is the
  `hadamard(b)` method, NOT `*`. `+ - /` are element-wise, same shape.
- **Element access = 2D `m[r][c]`.** `m[r]` → `Vector<T,C>` row (flat lanes
  `[r*C, r*C+C)`); `m[r][c]` reads/writes element at flat lane `r*C+c`.
- **Representation:** flat row-major `<R*C x T>` on host AND device; by-value /
  `PRIMITIVE`-marshalled like `Vector`. Element (r,c) = lane r*C+c.

## Status (2026-06-05) — S1–S7 DONE

- **S1 (DONE).** `MATRIX_FLAG` (bit 21); `CajetaMatrix` (`<R*C x T>`, mirrors
  `CajetaVector`); `fromContext` recognizes `Matrix<T,R,C>` references →
  `CajetaMatrix`; declared `runtime/src/cajeta/math/Matrix.cajeta` operator/method
  surface (placeholder bodies — generic templates aren't codegen'd until
  instantiated, and references resolve to `CajetaMatrix`, so the placeholders are
  never emitted). `STDLIB_STRUCTURE_COUNT` 101 → 102. Tests `MatrixTests.type*`.
- **S2+S3 (DONE).** `MatrixOps.h` (shared flat-vector intrinsics: build / get /
  set / row / col / transpose / identity / matmul / matVec / scale / hadamard).
  `new/stack Matrix<T,R,C>(R*C args)` → row-major value (`NewExpression`). `m[r]`
  → `Vector<T,C>` row (so `m[r][c]` reads via the vector index path); `m[r][c]=v`
  writes the matrix slot directly at flat lane `r*C+c` (`BinaryOpExpression`),
  not the row temporary. Dynamic indices supported.
- **S4 (DONE).** `+ - /` element-wise (same shape → `Matrix<T,R,C>`), `== !=`
  element-wise reduced to boolean (`AndReduce`), methods `transpose` / `identity`
  (square) / `row` / `col` / `hadamard` intercepted on a `CajetaMatrix` receiver
  in `MethodCallExpression`.
- **S5 (DONE).** `*` = matmul (`generateMatrixMul`): Matrix*Matrix (K checked) +
  Matrix*Vector + Matrix*scalar, `CAJETA_ERROR_MATRIX_SHAPE` on mismatch. This is
  the K/shape-generic operator the current overloading mechanism cannot express
  as one non-templated declaration — the motivating case for the method-templated
  -operator follow-on; here it is a codegen interception (like Vector).
- **S6 (DONE, CPU path).** Device lowering in `KernelLowering`: `deviceMatrixType`
  + `matrixShapes` (name → (R,C), since a `Matrix<2,3>` and `Vector<6>` share the
  `<6 x float>` slot type). `lowerNewMatrix`, `matrixIndexRead` (m[r][c]),
  `tryMatrixElementAssign`, `lowerMatrixMul` (* = matmul/matVec/scale) — all gated
  on `matrixShapes` so the Vector path is untouched; `+ - /` lower via the flat
  vector path. `XpuMatrixDeviceTests`: IR + CPU-oracle matmul/matVec/write.
- **S7 (DONE).** `CAJETA_ERROR_MATRIX_{ELEMENT_TYPE, DIMENSIONS, CONSTRUCT,
  METHOD, SHAPE}` wired + negative tests; full Matrix host+device suite green;
  Vector / operator-overload suites unchanged; `STDLIB_STRUCTURE_COUNT == 102`.

## New / changed files
- `src/cajeta/type/CajetaMatrix.{h,cpp}`, `src/cajeta/type/MatrixOps.h` (new).
- `runtime/src/cajeta/math/Matrix.cajeta` (new); `src/CMakeLists.txt` (+math dir).
- `src/cajeta/type/CajetaType.{h,cpp}` (`MATRIX_FLAG`, `fromContext` recognition).
- `src/cajeta/asn/expression/NewExpression.cpp` (construction),
  `Expression.cpp` (`m[r]` row read + resolveTypes),
  `BinaryOpExpression.{h,cpp}` (element-wise / == / matmul / m[r][c] write),
  `MethodCallExpression.cpp` (methods).
- `src/cajeta/xpu/lowering/KernelLowering.cpp` (device path).
- `test/expression/MatrixTests.cpp`, `test/xpu/XpuMatrixDeviceTests.cpp` (new);
  `test/compile/CompilerTests.cpp` (count 102).

## Follow-ons (NOT v1)
- **Device VK/AMD execution** of matrices (hardware-gated, like
  `XpuVectorDeviceTests.runsOn{Vulkan,Amd}Device`) and matrix kernel **params**
  (by-value marshalling) — S6 covers the CPU oracle + construction-as-locals.
- **Device matrix methods** (transpose/identity/row/col/hadamard in the kernel
  lowerer) — host has them; device v1 has construct + m[r][c] + arithmetic +
  matmul + matVec.
- **Real operator dispatch** (route the concrete `+ - == []` scalar `*` through
  `resolveMethod`/`invokeMethod` instead of interception) and
  **method-templated operators** (so K-generic `matmul` is a declared+dispatched
  `operator*`). Matrix is the motivating case (`OperatorOverloading.md` §8/§14).
- **`m[r][c]` static bounds check** (`CAJETA_ERROR_MATRIX_COMPONENT`) for constant
  indices — dynamic indices preclude full static checking, consistent with how
  Vector `[i]` is unchecked.
- determinant / inverse, `Quaternion`, swizzles, `cross`/`reflect`.

## Verification
- `./build.sh`; `CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test --gtest_filter=MatrixTests.*:XpuMatrixDeviceTests.*:CompilerTests.*`.
- Commit when complete + verified; brief messages; no attribution trailer; stage
  files explicitly; push stays ask-first.
