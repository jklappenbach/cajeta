# cajeta.math

This section measures tensors, linear algebra, FFT, random numbers, NumPy interchange and the small geometry types, against NumPy and SciPy first and the native libraries second.

| Page | |
|---|---|
| [Tensor](Tensor.md) | The keystone n-dimensional array |
| [LinAlg](linalg/LinAlg.md) | Dense linear algebra: solve, inverse, QR and SVD at 512 and 2048 |
| [Fft](fft/Fft.md) | The fast Fourier transform |
| [Generator](random/Generator.md) | Pseudo-random generation |
| [Npy](npio/Npy.md) | NumPy .npy interchange |
| [Poly](poly/Poly.md) | Polynomial evaluation and fitting |
| [Stats](stats/Stats.md) | Descriptive statistics |
| [Ray](Ray.md) | Ray intersection, the inner loop of a tracer |
| [Rotation](Rotation.md) | Rotations as quaternions and matrices |
| [Transform](Transform.md) | Affine transforms |

## Standing

Geometric mean 35x behind over 31 benchmarks, 2 wins and 29 losses. Transpose and rotation composition are wins or at parity; everything that touches tensor elements is 10x to 1000x behind, with LinAlg at 128x to 1000x and FFT at 144x to 309x. One cause: the cpu paths read every element through an out-of-line call (tensor-element-access, benchmark-reference 22.2.8). That is the cpu parity spec's first job and this package is where it shows.
