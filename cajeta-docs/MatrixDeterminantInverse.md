# Matrix determinant & inverse

`Matrix<T,R,C>` exposes `determinant()` and `inverse()` for **square** 2×2, 3×3,
and 4×4 matrices (the transform sizes), float element only. Both are compiler
intrinsics computed by cofactor expansion, fully unrolled, register-resident, and
identical on CPU / Vulkan / AMD.

```
float32 d = m.determinant();              // scalar
Matrix<float32,3,3> mi = m.inverse();     // adjugate / det
```

## Determinant — invertible? what does the transform scale?
A zero determinant means the matrix is **singular** (not invertible):
```
if (m.determinant() != 0.0f) { /* safe to invert */ }
```
`|det|` is the volume (3×3) / area (2×2) scale factor of the transform; a
negative determinant means it flips orientation (a mirror).

## Inverse — undo a transform, or solve a system
`A.inverse()` is the transform that undoes `A`; `A⁻¹ · b` solves `A · x = b`:
```
Vector<float32,3> x = a.inverse() * b;    // solve a·x = b for x
```
Round-trip sanity: `A * A.inverse()` is the identity.

**Singular input** (`det == 0`) yields inf/nan rather than an error — guard with
`determinant()` first if the matrix may be degenerate.

**Limits:** square 2×2 / 3×3 / 4×4, float only. Non-square or other sizes →
`CAJETA_ERROR_MATRIX_METHOD`. Larger systems (LU / Gaussian elimination) are a
follow-on. Runnable end to end in `samples/Tour` (the `LinearAlgebraDemo` host
walk and the XPU `linear-algebra` kernels).
