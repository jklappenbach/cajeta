# Matrix determinant & inverse

`Matrix<T,R,C>` exposes `determinant()` and `inverse()` for square 2×2, 3×3,
and 4×4 matrices, the transform sizes, with float elements only. Both are
compiler intrinsics computed by cofactor expansion. They are fully unrolled,
register-resident, and identical on CPU, Vulkan and AMD.

```
float32 d = m.determinant();              // scalar
Matrix<float32,3,3> mi = m.inverse();     // adjugate / det
```

## Determinant — invertible? what does the transform scale?
A zero determinant means the matrix is singular, so it cannot be inverted:
```
if (m.determinant() != 0.0f) { /* safe to invert */ }
```
`|det|` is the volume scale factor of the transform for 3×3 and the area scale
factor for 2×2. A negative determinant means the transform flips orientation,
as a mirror does.

## Inverse — undo a transform, or solve a system
`A.inverse()` is the transform that undoes `A`, and `A⁻¹ · b` solves `A · x = b`:
```
Vector<float32,3> x = a.inverse() * b;    // solve a·x = b for x
```
As a round-trip sanity check, `A * A.inverse()` is the identity.

Singular input (`det == 0`) yields inf/nan rather than an error. Guard with
`determinant()` first if the matrix may be degenerate.

The limits are square 2×2 / 3×3 / 4×4 and float only. Non-square or other
sizes report `CAJETA_ERROR_MATRIX_METHOD`. Larger systems (LU / Gaussian
elimination) are a follow-on. Both methods are runnable end to end in
`samples/Tour`, in the `LinearAlgebraDemo` host walk and the XPU
`linear-algebra` kernels.
