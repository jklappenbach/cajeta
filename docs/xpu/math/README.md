# Math: the tensor layer

`cajeta.math` is the numeric layer between the device substrate and
everything that computes with arrays: the n-dimensional `Tensor`, the dtype
system, dense linear algebra, the FFT, random generation, NumPy
interchange, polynomials and statistics, and the small geometry types. It
is what the LLM engine's activations are, what nucleo's columns alias, and
what the ML library's estimators consume.

## What it is

`Tensor<T>` is a strided view over a shared, reference-counted storage:
offset, shape and strides over one buffer, so a slice, a transpose or a
reshape is a new view and not a copy. The element type is a typed-width
primitive (`float32`, `float64`, `int32`, …) chosen at the type, and
`DType` names it at run time for code that moves between them. Host code
reads elements through `flatGet` and the raw `hostBuffer()`; device code
never sees a `Tensor`, it sees the `KernelBuffer` the tensor's storage was
uploaded to, which is the one seam between this layer and
[XPU](../README.md).

The packages beneath it: `linalg` (solve, inverse, QR, SVD, least squares),
`fft` (complex64 transforms), `random` (a float32 generator), `npio` (the
`.npy` format, load and save), `poly` (evaluation; fitting goes through
least squares), `stats` (moments, histograms), and `Ray`, `Rotation`,
`Transform`, `Camera`, `Color` for graphics and robotics code.

## How the layers above use it

- **The LLM engine** keeps its activations as `Tensor<float32>` on the host
  side of every step and uploads the packed forms to device buffers; a
  decode step touches no `Tensor` at all once the model is resident. The
  [walkthrough](../walkthrough.md) shows where the boundary sits.
- **Nucleo** aliases a `Column`'s buffer as a `Tensor` with no copy
  (`Column.asTensor`), which is how a dataframe column becomes an estimator's
  design matrix in one line.
- **ML** is written over `Tensor<float64>` for the classical half and
  `Tensor<float32>` for the neural half; the precision boundary is crossed
  once, at the estimator adapter.

## Where it stands

The [benchmarks](../../../bench/math/README.md) measure element access,
elementwise and reduction kernels, matmul, the linear algebra at 512 and
2048, the FFT, random draws, `.npy` interchange and the geometry types
against NumPy and SciPy, Eigen, FFTW, ndarray and gonum. Several pages say
what the library lacks today and state the nearest honest measurement: no
tensor cast, no polynomial fit, no skew or kurtosis, float32-only random and
FFT, a translation-rotation-scale `Transform` rather than a 4×4.

## Reference

The stdlib pages, one per class: [Tensor](../../stdlib/math/Tensor.md),
[DType](../../stdlib/math/DType.md), [LinAlg](../../stdlib/math/linalg/LinAlg.md),
[Fft](../../stdlib/math/fft/Fft.md), [Generator](../../stdlib/math/random/Generator.md),
[Npy](../../stdlib/math/npio/Npy.md), [Poly](../../stdlib/math/poly/Poly.md),
[Stats](../../stdlib/math/stats/Stats.md), [Ray](../../stdlib/math/Ray.md),
[Rotation](../../stdlib/math/Rotation.md), [Transform](../../stdlib/math/Transform.md),
[Camera](../../stdlib/math/Camera.md), [Color](../../stdlib/math/Color.md).
