# CajetaML.md

A design for `cajeta-ml`, a separate-from-stdlib library scoped to make
cajeta a natural choice for machine-learning research and production:
n-dimensional tensors, linear algebra, statistics, regression and
classical ML, automatic differentiation, neural networks, geometry
primitives. Numpy + scikit-learn + a workable autograd / NN layer, all
in one tree.

This document also captures the prerequisite **cajeta.math expansion**:
boxed Object equivalents for every native numeric type (including the
fp4 / fp6 / fp8 variants the language already lowers), plus the
precision-aware casting and interop surface that ML workloads need.

Implementation lands incrementally as separate `.cajeta` files, the
math foundation under `./runtime/src/cajeta/math/`, the ML library
under `./libraries/cajeta-ml/src/` (separate tree — `cajeta-ml` ships
as its own package, not a stdlib component).

## Why a separate library

The stdlib answers "what does every cajeta program need." That answer
is small: error types, strings, time, collections, basic IO. Tensors,
autograd, optimizers, and regression models don't belong there — most
programs never touch them, the surface area is huge, the dependency
weight is real (BLAS, LAPACK, eventually accelerator runtimes), and
the API churn cycle is faster than stdlib should accept.

Keeping `cajeta-ml` separate means: independent versioning, optional
dependency on accelerator backends, room for the API to evolve without
stdlib stability constraints, and clarity for users about what they're
opting into.

The math foundation (boxed primitives, precision-aware casting) stays
in `cajeta.math` because every program with a hash map of doubles
benefits from `Double` being a real Object — not just ML programs.

## Goals (cajeta-ml)

- **Numpy parity for the everyday surface.** Arithmetic, broadcasting,
  reshape, slicing, reductions, linalg, fft, random, basic stats. If a
  numpy script translates 1:1 in spirit, that's a win.
- **First-class autograd.** Forward and reverse mode, dynamic graphs
  (PyTorch shape, not TensorFlow 1.x), so research code can introspect
  and modify computation at runtime.
- **Classical ML covered.** Regression (linear / logistic /
  polynomial), clustering (k-means, hierarchical, DBSCAN),
  classification (kNN, SVM, naive Bayes), trees (decision trees,
  random forests, gradient boosting). The scikit-learn estimator
  contract: `fit(X, y) -> Self`, `predict(X) -> Y`, `score(X, y)`.
- **NN building blocks.** Layers, optimizers, loss functions,
  parameter management, training loops. The PyTorch `nn.Module` shape
  fits here cleanly.
- **Geometry that ML/graphics actually use.** Quaternions, SE(3) /
  SO(3) transforms, rotation conversions, spatial vector types.
- **Mixed precision is native, not an afterthought.** Tensors carry
  dtype; fp4/fp6/fp8/fp16/bf16/fp32/fp64 all participate. Casting
  between them is explicit and uses the math foundation's casting API.

## Non-goals (v1)

- **GPU / accelerator acceleration.** CPU-only first. The tensor
  abstraction leaves room for a backend (CUDA / Metal / Vulkan /
  OpenCL / oneAPI) but v1 lowers everything through the cajeta runtime
  on CPU, optionally vectorized via LLVM intrinsics.
- **Distributed training.** Multi-host / multi-GPU coordination
  belongs in a follow-up library that builds on `cajeta-ml`.
- **Pre-trained model zoo.** No bundled weights. The infrastructure
  supports loading from external sources (npy / safetensors / ONNX
  later); we don't curate a model library.
- **JIT graph compilers.** No XLA-equivalent in v1. Eager execution
  with vectorized kernels is the baseline; tracing / compilation is a
  later layer.
- **Probabilistic programming framework.** Distributions exist
  (`stats.distributions`); a Pyro/Stan-equivalent inference engine
  doesn't.

---

## Prerequisite: cajeta.math expansion

The current `cajeta.math` is a one-line entry in `StandardLibrary.md`
("Integer / Long / Double parse helpers, BigInteger, basic math
utility methods"). For ML use it needs to grow into the home for
**every** numeric type's boxed Object form, and into the precision-
aware casting / inspection surface that mixed-precision workloads
depend on.

### Boxed primitives — full coverage

Every native type gets an Object wrapper. Wrappers carry the value
plus a stable, well-defined API for inspection, casting, formatting,
parsing, and bit-level access.

```
cajeta.math.Int8        (boxed int8)       cajeta.math.UInt8
cajeta.math.Int16       (boxed int16)      cajeta.math.UInt16
cajeta.math.Int32       (boxed int32)      cajeta.math.UInt32
cajeta.math.Int64       (boxed int64)      cajeta.math.UInt64
cajeta.math.Int128      (boxed int128)     cajeta.math.UInt128

cajeta.math.Float4E2M1     (boxed float4e2m1)
cajeta.math.Float6E2M3     (boxed float6e2m3)
cajeta.math.Float6E3M2     (boxed float6e3m2)
cajeta.math.Float8E4M3     (boxed float8e4m3)
cajeta.math.Float8E5M2     (boxed float8e5m2)
cajeta.math.Float8E4M3FNUZ (boxed float8e4m3fnuz)
cajeta.math.BFloat16       (boxed bfloat16)
cajeta.math.Float16        (boxed float16)
cajeta.math.Float32        (boxed float32)
cajeta.math.Float64        (boxed float64)
```

`cajeta.lang.Integer / Long / Double / Boolean` (the shapes already
documented in StandardLibrary.md) become aliases / re-exports of
`cajeta.math.Int32 / Int64 / Float64 / Boolean`. The math package is
the source of truth; cajeta.lang carries the Java-style names for
ergonomics.

### Common shape — every boxed numeric type

```cajeta
public final class Float8E4M3 implements Comparable<Float8E4M3> {
    // The wrapped value. Public so the unboxing path is a single
    // field access; the compiler can fold (new Float8E4M3(x)).value
    // into a no-op.
    public float8e4m3 value;

    public Float8E4M3(float8e4m3 v);

    // ----- precision-up casting (always lossless) -----
    public Float16  toFloat16();
    public BFloat16 toBFloat16();
    public Float32  toFloat32();
    public Float64  toFloat64();

    // ----- precision-down casting (lossy; explicit rounding) -----
    public Float4E2M1 toFloat4E2M1(RoundingMode mode = RoundingMode.NEAREST_EVEN);
    public Float6E2M3 toFloat6E2M3(RoundingMode mode = RoundingMode.NEAREST_EVEN);
    public Float6E3M2 toFloat6E3M2(RoundingMode mode = RoundingMode.NEAREST_EVEN);

    // ----- sibling-precision casting (re-encode 8-bit family) -----
    public Float8E5M2     toFloat8E5M2(RoundingMode mode = RoundingMode.NEAREST_EVEN);
    public Float8E4M3FNUZ toFloat8E4M3FNUZ(RoundingMode mode = RoundingMode.NEAREST_EVEN);

    // ----- integer cast (truncates toward zero unless mode says otherwise) -----
    public Int8  toInt8(RoundingMode mode = RoundingMode.TRUNCATE);
    public Int32 toInt32(RoundingMode mode = RoundingMode.TRUNCATE);
    public Int64 toInt64(RoundingMode mode = RoundingMode.TRUNCATE);

    // ----- IEEE 754 inspection -----
    public boolean isNaN();
    public boolean isInfinite();
    public boolean isFinite();
    public boolean isSubnormal();
    public boolean isZero();
    public boolean isNegative();
    public int8    sign();          // -1, 0, +1
    public int8    exponent();      // raw biased exponent
    public int8    mantissa();      // raw mantissa bits

    // ----- bit-level access (interop with non-cajeta libraries) -----
    public int8    rawBits();                         // bit pattern as int8
    public byte[1] rawBytes();                        // bit pattern as bytes
    public static Float8E4M3 fromRawBits(int8 bits);  // round-trip
    public static Float8E4M3 fromRawBytes(byte[1] b);

    // ----- formatting / parsing -----
    public String toString();
    public String toString(int8 maxFractionDigits);
    public static Float8E4M3 parse(String s);
    public static Float8E4M3 parse(String s, RoundingMode mode);

    // ----- saturating arithmetic (overflow saturates to MAX/MIN) -----
    public Float8E4M3 saturatingAdd(Float8E4M3 other);
    public Float8E4M3 saturatingSub(Float8E4M3 other);
    public Float8E4M3 saturatingMul(Float8E4M3 other);

    // ----- constants -----
    public static Float8E4M3 NaN;
    public static Float8E4M3 POSITIVE_INFINITY;
    public static Float8E4M3 NEGATIVE_INFINITY;
    public static Float8E4M3 MAX_VALUE;
    public static Float8E4M3 MIN_VALUE;             // smallest positive normal
    public static Float8E4M3 MIN_SUBNORMAL_VALUE;
    public static Float8E4M3 EPSILON;               // ulp(1)
    public static Float8E4M3 ZERO;
    public static Float8E4M3 NEGATIVE_ZERO;

    // ----- Object overrides -----
    public boolean operator==(Object obj);
    public int64   hash();
    public int32   compare(Float8E4M3 other);
}
```

The same shape applies to every floating-point box — only the
constants and the casting destinations differ. Integer boxes drop
the IEEE inspection methods, gain bit-manipulation helpers
(`bitCount`, `numberOfLeadingZeros`, `reverse`, etc.), and replace
saturating arithmetic with two-variant arithmetic (wrapping +
checked).

### `RoundingMode`

```cajeta
public enum RoundingMode {
    TRUNCATE,        // toward zero
    FLOOR,           // toward -inf
    CEIL,            // toward +inf
    NEAREST_EVEN,    // banker's rounding (IEEE 754 default)
    NEAREST_AWAY,    // away from zero on .5
    STOCHASTIC       // weighted by distance — important for fp8 training
}
```

`STOCHASTIC` rounding is non-deterministic by design (samples from the
RNG returned by `cajeta-ml.random.default()`); it's the standard
technique for low-precision training to avoid systematic bias when
quantizing gradients and weights.

### Why fp4/fp6/fp8 deserve dedicated boxes

These formats have small dynamic range and tight quantization steps;
the failure modes (overflow to inf, underflow to zero, NaN propagation,
denormal flushing) bite immediately when they're used naively. The
boxed types make the gotchas inspectable (`isSubnormal()`,
`saturatingAdd`) and the casts to and from larger formats explicit.
Without that surface, fp8 training loops degrade silently in ways
that take days to debug.

The boxed types also give us the natural place to hang the conversion
correctness tests — a precision-down cast from Float64 → Float8E4M3
has well-defined behavior we can pin down once and trust everywhere.

### Arbitrary-precision types

```
cajeta.math.BigInteger     — arbitrary-precision signed integer
cajeta.math.BigDecimal     — arbitrary-precision decimal with scale
cajeta.math.Rational       — exact ratio of two BigIntegers
```

Standard arithmetic, comparisons, conversions to/from boxed primitives
(with explicit rounding when the destination can't represent the
value), parsing, formatting. Same shape as Java's `BigInteger /
BigDecimal` so users coming from JVM languages get a familiar API.

### Numeric utilities

```
cajeta.math.Math       — sin, cos, exp, log, pow, sqrt, etc.
                         (already partly intrinsic; Math is the
                         documented surface, intrinsics are the
                         implementation strategy)
cajeta.math.bit        — popcount, leading-zeros, byte-swap helpers
                         (currently scattered across Integer / Long;
                         consolidate)
cajeta.math.constants  — PI, E, TAU, GOLDEN, LN2, LN10, etc. as
                         Float64 constants
```

---

## cajeta-ml package layout

```
cajeta-ml.tensor          — N-d arrays with dtype, shape, strides;
                             broadcasting, slicing, reductions; the numpy core
cajeta-ml.linalg          — matmul, dot, decompositions (LU, QR, SVD,
                             Cholesky, eigvals), solvers, norms,
                             pseudoinverse
cajeta-ml.random          — RNGs (PCG, Xoshiro), distributions
                             (uniform, normal, gamma, beta, multinomial,
                             dirichlet, ...), seeding + reproducibility
cajeta-ml.stats           — descriptive (mean, std, var, quantiles,
                             skew, kurtosis), distributions (pdf/cdf/
                             ppf), hypothesis tests (t, chi-sq, KS,
                             Mann-Whitney, ANOVA), correlation, EWMA
cajeta-ml.fft             — forward / inverse FFT (1D / 2D / N-D),
                             real FFT, DCT, windowing
cajeta-ml.regression      — linear, ridge, lasso, elastic-net, logistic,
                             polynomial; sklearn-shaped estimators
cajeta-ml.cluster         — k-means, k-means++, mini-batch k-means,
                             hierarchical (single/complete/average/Ward),
                             DBSCAN, OPTICS, GMM
cajeta-ml.classify        — kNN, SVM (linear / kernel), naive Bayes
                             (gaussian / multinomial / bernoulli),
                             discriminant analysis
cajeta-ml.tree            — decision trees, random forests, extra
                             trees, gradient boosting (sklearn-shape)
cajeta-ml.preprocess      — scaling (standard, min-max, robust),
                             encoding (one-hot, ordinal), imputation,
                             PCA, t-SNE, UMAP
cajeta-ml.metrics         — accuracy, precision, recall, F1, AUC,
                             MSE, MAE, R^2, log-loss, confusion matrix
cajeta-ml.autograd        — Tensor with grad tracking, backward(),
                             grad accumulation, no-grad scopes,
                             checkpointing
cajeta-ml.nn              — Module base, layers (Linear, Conv*, BN,
                             LayerNorm, Dropout, Embedding, attention),
                             activations, losses, optimizers (SGD, Adam,
                             AdamW, RMSProp, LAMB), LR schedulers,
                             parameter init
cajeta-ml.geom            — quaternion, vec2/vec3/vec4, mat2/mat3/mat4,
                             SE3 / SO3, axis-angle, Euler angles,
                             slerp, quaternion <-> matrix conversions
cajeta-ml.io              — npy / npz read+write (numpy-compatible),
                             safetensors (later), csv loaders, image
                             loaders (later)
cajeta-ml.signal          — convolution, correlation, filter design,
                             windowing, resampling
cajeta-ml.optim           — separate from cajeta-ml.nn.optim — generic
                             scalar / vector optimization (BFGS,
                             L-BFGS, Nelder-Mead, simulated annealing,
                             differential evolution)
```

---

## cajeta-ml.tensor

The foundational type. Everything in cajeta-ml flows through Tensor.

```cajeta
public final class Tensor<T> implements Collection<T> {
    // Construction
    public static Tensor<T> zeros(Shape shape);
    public static Tensor<T> ones(Shape shape);
    public static Tensor<T> full(Shape shape, T value);
    public static Tensor<T> arange(T start, T stop, T step);
    public static Tensor<T> linspace(T start, T stop, int64 num);
    public static Tensor<T> fromArray(Array<T> data, Shape shape);

    // Inspection
    public Shape       shape();
    public Strides     strides();
    public DType       dtype();
    public int64       size();        // total element count
    public int8        rank();        // shape.size()

    // Element access (zero-copy views)
    public Tensor<T> at(int64... indices);   // fancy indexing
    public Tensor<T> slice(Range... ranges);
    public Tensor<T> reshape(Shape newShape);
    public Tensor<T> transpose(int8... axes);
    public Tensor<T> permute(int8... axes);
    public Tensor<T> squeeze(int8 axis);
    public Tensor<T> unsqueeze(int8 axis);
    public Tensor<T> broadcast(Shape target);

    // Type casting (precision-aware via cajeta.math casting)
    public <U> Tensor<U> astype(DType targetDtype, RoundingMode mode = RoundingMode.NEAREST_EVEN);

    // Element-wise arithmetic (broadcast)
    public Tensor<T> operator+(Tensor<T> other);
    public Tensor<T> operator-(Tensor<T> other);
    public Tensor<T> operator*(Tensor<T> other);
    public Tensor<T> operator/(Tensor<T> other);

    // Reductions
    public Tensor<T> sum(int8... axes);
    public Tensor<T> mean(int8... axes);
    public Tensor<T> max(int8... axes);
    public Tensor<T> min(int8... axes);
    public Tensor<T> argmax(int8 axis);
    public Tensor<T> argmin(int8 axis);

    // Boolean / mask
    public Tensor<boolean> operator>(Tensor<T> other);
    public Tensor<boolean> operator==(Tensor<T> other);
    public Tensor<T>       where(Tensor<boolean> mask, Tensor<T> other);

    // Materialization
    public Array<T> toArray();
    public T        scalar();        // for 0-d tensors
}

public final class Shape {
    public Shape(int64... dims);
    public int8  rank();
    public int64 dimAt(int8 axis);
    public int64 totalSize();
}

public final class DType {
    // Static instances for every supported element type.
    public static DType INT8;
    public static DType INT16;
    public static DType INT32;
    public static DType INT64;
    public static DType UINT8;
    public static DType FLOAT4_E2M1;
    public static DType FLOAT6_E2M3;
    public static DType FLOAT6_E3M2;
    public static DType FLOAT8_E4M3;
    public static DType FLOAT8_E5M2;
    public static DType FLOAT8_E4M3FNUZ;
    public static DType BFLOAT16;
    public static DType FLOAT16;
    public static DType FLOAT32;
    public static DType FLOAT64;
    public static DType COMPLEX64;
    public static DType COMPLEX128;
    public static DType BOOL;

    public int8   bytesPerElement();
    public boolean isFloat();
    public boolean isInteger();
    public boolean isComplex();
    public boolean isSigned();
}
```

Implementation strategy: storage is a single heap buffer (`byte[]`)
sized to `dtype.bytesPerElement() * shape.totalSize()`, plus a small
header for shape / strides / offset. Views (slice / reshape /
transpose) share the underlying buffer; only `astype` and explicit
copies allocate.

`Tensor<T>` parameterizing on the cajeta-ml.tensor element type
(`int32`, `float32`, etc., not the boxed forms) lets the compiler
specialize hot kernels per dtype. The `DType` runtime tag covers the
generic-over-dtype API surface (`astype`, IO).

---

## cajeta-ml.autograd

PyTorch-style dynamic graphs. A `Tensor` carries a `grad` field and a
`gradFn` reference; operations build the graph implicitly; `backward()`
walks it in reverse.

```cajeta
public final class Variable<T> {
    public Tensor<T>  data;
    public Tensor<T>  grad;
    public boolean    requiresGrad;
    public GradFn     gradFn;        // null for leaves

    public Variable(Tensor<T> data, boolean requiresGrad);

    public void backward(Tensor<T> outputGrad = null);
    public void zeroGrad();
    public Variable<T> detach();
}

public class NoGradScope implements AutoCloseable {
    public NoGradScope();
    public void close();
}
```

Op authors register a `Function<Inputs, Output, GradInputs>` that
provides forward + backward. Standard ops (matmul, add, mul, relu,
softmax, log, sum, mean, ...) ship as built-ins.

---

## cajeta-ml.nn

```cajeta
public abstract class Module {
    // Subclasses override to define the forward pass.
    public abstract Variable<float32> forward(Variable<float32>... inputs);

    // Parameter discovery — walks declared fields of type Variable
    // and collects them, recursing into sub-Modules. The compiler
    // synthesizes the walk via @Trainable field metadata.
    public Array<Variable<float32>> parameters();

    public void train();
    public void eval();
    public boolean isTraining();

    public void saveTo(String path);
    public void loadFrom(String path);
}

public class Linear extends Module {
    public Variable<float32> weight;
    public Variable<float32> bias;

    public Linear(int64 inFeatures, int64 outFeatures, boolean bias = true);
    public Variable<float32> forward(Variable<float32> x);
}

public abstract class Optimizer {
    public abstract void step();
    public abstract void zeroGrad();
}

public class Adam extends Optimizer {
    public Adam(Array<Variable<float32>> params,
                float64 lr = 1e-3,
                float64 beta1 = 0.9,
                float64 beta2 = 0.999,
                float64 eps = 1e-8,
                float64 weightDecay = 0.0);
    public void step();
    public void zeroGrad();
}
```

Layer set for v1: `Linear`, `Conv1d`, `Conv2d`, `Conv3d`, `BatchNorm{1,2,3}d`,
`LayerNorm`, `RMSNorm`, `Dropout`, `Embedding`, `MultiheadAttention`,
`TransformerEncoderLayer`, `TransformerDecoderLayer`. Activations as
Module-or-function pairs: `ReLU`, `GELU`, `SiLU`, `Tanh`, `Sigmoid`,
`Softmax`. Losses: `MSELoss`, `CrossEntropyLoss`, `BCELoss`, `L1Loss`,
`SmoothL1Loss`, `HuberLoss`, `KLDivLoss`. Optimizers: `SGD` (with
momentum + Nesterov), `Adam`, `AdamW`, `RMSProp`, `Adagrad`, `LAMB`.

---

## cajeta-ml.regression — sklearn estimator contract

```cajeta
public interface Estimator<X, Y> {
    public Self    fit(X data, Y target);
    public Y       predict(X data);
    public float64 score(X data, Y target);
}

public class LinearRegression implements Estimator<Tensor<float64>, Tensor<float64>> {
    public Tensor<float64> coefficients;
    public float64         intercept;
    public boolean         fitIntercept;

    public LinearRegression(boolean fitIntercept = true);
    public LinearRegression fit(Tensor<float64> X, Tensor<float64> y);
    public Tensor<float64>  predict(Tensor<float64> X);
    public float64          score(Tensor<float64> X, Tensor<float64> y);   // R^2
}
```

Same shape for `Ridge`, `Lasso`, `ElasticNet`, `LogisticRegression`,
`PolynomialRegression`. The estimator interface is the contract every
classical-ML model honors so users can swap implementations cleanly.

---

## cajeta-ml.geom — quaternions and transforms

```cajeta
public final class Quaternion<T> {
    public T w;
    public T x;
    public T y;
    public T z;

    public Quaternion(T w, T x, T y, T z);
    public static Quaternion<T> identity();
    public static Quaternion<T> fromAxisAngle(Vec3<T> axis, T angleRadians);
    public static Quaternion<T> fromEuler(T roll, T pitch, T yaw);
    public static Quaternion<T> fromMatrix(Mat3<T> m);
    public static Quaternion<T> fromTwoVectors(Vec3<T> from, Vec3<T> to);

    public Quaternion<T> operator*(Quaternion<T> other);   // composition
    public Vec3<T>       operator*(Vec3<T> v);             // rotate vector
    public Quaternion<T> inverse();
    public Quaternion<T> conjugate();
    public Quaternion<T> normalize();
    public T             norm();
    public T             dot(Quaternion<T> other);

    public Mat3<T> toMatrix();
    public Vec3<T> toEuler();         // roll, pitch, yaw

    public static Quaternion<T> slerp(Quaternion<T> a, Quaternion<T> b, T t);
    public static Quaternion<T> nlerp(Quaternion<T> a, Quaternion<T> b, T t);
}

public final class Vec3<T> { public T x; public T y; public T z; /* + arithmetic */ }
public final class Vec4<T> { public T x; public T y; public T z; public T w; }
public final class Mat3<T> { /* 3x3 */ }
public final class Mat4<T> { /* 4x4 — homogeneous transforms */ }

public final class SE3<T> {
    public Mat3<T> rotation;        // SO(3)
    public Vec3<T> translation;
    /* compose, inverse, applyTo(Vec3), applyTo(SE3), interpolate, ... */
}
```

Generic over `T` so `Quaternion<float32>` (graphics) and
`Quaternion<float64>` (control / robotics) both work without a second
implementation.

---

## Implementation sequence

A reasonable order, given dependencies:

1. **cajeta.math expansion: boxed primitives + RoundingMode + casting
   API.** No cajeta-ml code can land cleanly without these. The
   intrinsic-fp tests already in the suite (`Fp*Tests`) cover the
   underlying lowering; this step adds the user-visible Object API.
2. **cajeta.math: BigInteger, BigDecimal, Rational.** Standalone,
   useful outside ML, lets later steps reuse the arbitrary-precision
   path (e.g. exact statistical computations).
3. **cajeta-ml.tensor.** The foundation. Strided storage, broadcasting,
   slicing, reductions, the dtype tag. Element-wise ops as a baseline
   (no autograd yet, no linalg yet).
4. **cajeta-ml.linalg.** matmul + the basic decompositions. Wraps
   reference-implementation kernels initially; LAPACK / OpenBLAS
   backend can plug in later behind the same surface.
5. **cajeta-ml.random + cajeta-ml.stats.** Self-contained, low
   dependency, useful immediately. Distributions need `linalg` for
   multivariate cases.
6. **cajeta-ml.io: npy / npz.** Interop with the existing numpy
   ecosystem unblocks dataset use. Single-file format, well-specified.
7. **cajeta-ml.regression + cajeta-ml.metrics.** Classical ML, no
   autograd needed, exercises the estimator contract end-to-end.
8. **cajeta-ml.cluster + cajeta-ml.classify + cajeta-ml.tree.**
   Round out the sklearn-equivalent surface. Each is independent;
   ship as you go.
9. **cajeta-ml.geom.** Quaternions, Vec/Mat, SE3. Independent of the
   rest of the stack; can land in parallel with classical ML.
10. **cajeta-ml.autograd.** The Variable wrapper, backward graph,
    function registration. Built on `cajeta-ml.tensor`.
11. **cajeta-ml.nn.** Layers, optimizers, training loop. Built on
    autograd. Transformer block early so language-model workloads
    are reachable.
12. **cajeta-ml.fft + cajeta-ml.signal.** Numerical-methods-flavored
    additions. Independent of the ML stack.
13. **cajeta-ml.preprocess.** PCA / t-SNE / UMAP. Needs `linalg`
    + `random`. Round out for end-to-end ML pipelines.
14. **Accelerator backend (separate effort).** Replace the CPU
    kernels behind an abstraction so the same `cajeta-ml.tensor` API
    targets GPU / NPU. Out of scope for v1.

The gating step is (1) — every other layer touches boxed numerics,
casting, and rounding. It's small in surface area but foundational.

---

## Open questions

- **Boxed primitive locations.** Keep `cajeta.lang.Integer / Long /
  Double` as the everyday names and treat `cajeta.math.Int32 /
  Int64 / Float64` as canonical, or fold the everyday names into
  `cajeta.math` directly? Java's split has historical baggage; we
  could pick the cleaner shape.
- **DType representation.** Runtime-tag (`DType`) on a tensor
  generic over `T` produces a small inconsistency: `Tensor<float32>`
  knows its element type at compile time, but `astype` returns a
  tensor with the runtime-chosen dtype. Worth resolving — probably
  with a separate `AnyTensor` base type that erases the element type
  for the dtype-generic API.
- **Stochastic rounding RNG sourcing.** `RoundingMode.STOCHASTIC`
  needs an RNG; current sketch reaches into
  `cajeta-ml.random.default()`. That couples cajeta.math to
  cajeta-ml. Alternative: stochastic rounding API takes an explicit
  RNG argument and isn't a `RoundingMode` enum value at all (it's a
  runtime-only mode that can't be requested via simple casts). Worth
  a decision before locking the API.
- **Autograd graph storage.** Eager build (every op records into
  the current graph) vs. trace-on-demand (record only inside an
  explicit `withGrad { }` scope). Eager matches PyTorch and is
  intuitive; trace-on-demand is cheaper when most code is forward-
  only. Defer until the autograd implementation starts.
- **Module parameter discovery.** Synthesize via `@Trainable`
  annotation walked by the compiler, or by reflection at runtime?
  Compiler-walked is faster and statically-checked; runtime is
  flexible and matches the Python ergonomics ML researchers expect.
  Worth picking a side once `nn.Module` lands.
