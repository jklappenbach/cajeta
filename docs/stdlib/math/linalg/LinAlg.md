# LinAlg

`cajeta.math.linalg.LinAlg` — native-cajeta linear-algebra factorizations
over the Tensor/GEMM core, no foreign BLAS/LAPACK. `solve`/`det`/`inv` are
the Gaussian-elimination foundation (LU with partial pivoting);
`lu`/`qr`/`cholesky`/`eigh`/`svd` and the SVD-derived helpers build on it.
`E` is `Floating` (elimination needs division); matrices are 2-D row-major
`Tensor<E>`.

```cajeta
package snip.linalg;

import cajeta.math.Tensor;
import cajeta.math.linalg.LinAlg;

public final class Demo {
    public static void run() {
        int64[] shp = heap int64[2];
        shp[0] = 2;
        shp[1] = 2;
        Tensor<float32> a #= Tensor.zeros<float32>(shp);
        a.set2(0, 0, 4.0f);
        a.set2(0, 1, 1.0f);
        a.set2(1, 0, 1.0f);
        a.set2(1, 1, 3.0f);
        float32 d = LinAlg.det<float32>(a);          // 11
        Tensor<float32> ai #= LinAlg.inv<float32>(a);
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `static #Tensor<E> solve<E extends Floating>(Tensor<E> a, Tensor<E> b)` ⚑ | Solve the square system `A·x = b` (numpy `linalg.solve`): Gaussian elimination with partial pivoting + back-substitution |
| `static E det<E extends Floating>(Tensor<E> a)` ⚑ | Determinant of a square `A` (numpy `linalg.det`): product of the LU pivots, sign flipped per row swap |
| `static #Tensor<E> inv<E extends Floating>(Tensor<E> a)` ⚑ | Inverse of a square `A` (numpy `linalg.inv`): solve `A·X = I` column by column |
| `static #Tensor<E> cholesky<E extends Floating>(Tensor<E> a)` | Cholesky factorization of a symmetric positive-definite `A`: the lower-triangular `L` with `L·Lᵀ = A` |
| `static #Tensor<E>[] lu<E extends Floating>(Tensor<E> a)` | LU with partial pivoting: `[P, L, U]` with `A = P·L·U`, `L` unit lower-triangular, `U` upper-triangular |
| `static #Tensor<E>[] qr<E extends Floating>(Tensor<E> a)` | QR factorization (numpy `linalg.qr`, reduced for square `A`): `[Q, R]` with `A = Q·R` |
| `static #Tensor<E>[] eigh<E extends Floating>(Tensor<E> a)` | Symmetric eigendecomposition by cyclic Jacobi rotations: `[w, V]` with ascending eigenvalues `w` and orthonormal eigenvector columns `V` |
| `static #Tensor<E>[] svd<E extends Floating>(Tensor<E> a)` ⚑ | Singular value decomposition (full square case): `[U, S, Vt]` with `A = U·diag(S)·Vt`, `S` descending |
| `static #Tensor<E> pinv<E extends Floating>(Tensor<E> a)` | Moore-Penrose pseudo-inverse: `A⁺ = V·diag(1/σ)·Uᵀ`, singular values `σ ≤ tol` zeroed (`tol = σmax·1e-4`) |
| `static #Tensor<E> lstsq<E extends Floating>(Tensor<E> a, Tensor<E> b)` | Least-squares solution of `A·x ≈ b` via the pseudo-inverse: `x = A⁺·b` |
| `static int64 matrixRank<E extends Floating>(Tensor<E> a)` | Rank of a square `A`: count of singular values above `tol = σmax·1e-4` |
| `static E cond<E extends Floating>(Tensor<E> a)` | 2-norm condition number: `σmax / σmin` |
| `static E normFro<E extends Floating>(Tensor<E> a)` | Frobenius norm of a square `A`: `sqrt(Σ aᵢⱼ²)` |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/linalg/LinAlg.cajeta`](../../../../runtime/src/cajeta/math/linalg/LinAlg.cajeta)
- [Tensor](../Tensor.md) — `matmul`/`dot` and the other products live on the core type
