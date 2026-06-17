# CajetaMath.md

A design for `cajeta.math`, the numerical foundation library for
cajeta — and the layer everything else numerical builds on.
N-dimensional tensors, linear algebra, statistics (descriptive,
inferential, classical ML), signal processing, neural networks,
quaternions and spatial transforms, mixed-precision casting, RNGs,
arbitrary-precision arithmetic. NumPy + SciPy + scikit-learn-shaped
contracts in one tree, with autograd + neural-network plumbing as one
sub-package alongside the others.

This document covers two layers in one:

1. **Top-level `cajeta.math`** — boxed numeric Objects for every
   native type (including the fp4 / fp6 / fp8 variants the language
   already lowers), precision-aware casting, `RoundingMode`,
   arbitrary-precision (`BigInteger`, `BigDecimal`, `Rational`), and
   general-purpose RNGs. The compiler can fold the boxed forms back
   into their primitives in tight loops; the boxes exist so that
   `HashMap<Double, V>` works the way every other reference-typed
   keyed collection works.
2. **Sub-packages** — `cajeta.math.tensor`, `linalg`, `stats`,
   `signal`, `nn`. Each is an independent block on top of the boxed-
   primitive foundation.

Implementation lands incrementally as `.cajeta` files. The boxed-
primitive foundation lives under `./runtime/src/cajeta/math/` (stdlib-
adjacent — every program with a `HashMap<Double, V>` benefits from
boxed `Double`). The sub-packages live under `./libraries/cajeta.math/src/`
(separate tree; ships as its own package outside the stdlib so the
surface can evolve without stdlib-stability constraints).

## Why this is `cajeta.math`, not `cajeta.ml`

The math library is broader than ML. Game-engine physics needs
quaternions, dual quaternions, and rigid-body transforms.
Signal-processing software needs FFTs, convolution, and filter
design. Scientific computing needs linear algebra, statistics, and
arbitrary precision. Robotics needs SE(3) and Lie-algebra
exponentials. Audio needs cepstral analysis. Computer graphics needs
matrix decompositions for IK and skinning. ML uses every one of
these — but so does everyone else, and the namespace should reflect
that.

So `cajeta.math.*` is the long-lived numerical foundation; `cajeta.math.nn`
is the neural-network corner of it. Users who never touch a neural net
still get tensors, linalg, stats, and signal processing under a name
that doesn't presume otherwise.

The previous `cajeta.ml` framing is retired. `cajeta.math.nn`
absorbs autograd + layers + optimizers; `cajeta.math.stats` absorbs
the classical-ML estimators (regression, classification, clustering,
trees, preprocessing); `cajeta.math.signal` absorbs FFT and filter
design. The packages aren't moved out of view — they're recategorized
under their actual subject.

## Why a separate library (sub-packages, not stdlib)

The stdlib answers "what does every cajeta program need." That answer
is small: error types, strings, time, collections, basic IO. Tensors,
autograd, optimizers, and regression models don't belong there — most
programs never touch them, the surface area is huge, the dependency
weight is real (BLAS, LAPACK, eventually accelerator runtimes), and
the API churn cycle is faster than stdlib should accept.

The boxed-primitive foundation at the top level of `cajeta.math` is
different — every program with a hash map of doubles, a typed-
identifier system, or a parsing surface that returns "this Object is
some kind of number" benefits from it. That layer stays in the
stdlib-adjacent tree.

The sub-packages (tensor / linalg / stats / signal / nn) live as
separate libraries with independent versioning, optional dependencies
on the accelerator foundation ([`cajeta.gpu`](gpu/CajetaGPU.md) —
see §"Backend strategy"), and room for the APIs to evolve.

## Goals

- **NumPy parity for the everyday surface.** Arithmetic,
  broadcasting, reshape, slicing, reductions, linalg, FFT, random,
  basic stats. If a NumPy script translates 1:1 in spirit, that's a
  win.
- **First-class autograd.** Forward and reverse mode, dynamic graphs
  (PyTorch shape, not TensorFlow 1.x), so research code can introspect
  and modify computation at runtime.
- **Classical ML covered under stats.** Regression (linear / logistic
  / polynomial), clustering (k-means, hierarchical, DBSCAN),
  classification (kNN, SVM, naive Bayes), trees (decision trees,
  random forests, gradient boosting). The scikit-learn estimator
  contract: `fit(X, y) -> Self`, `predict(X) -> Y`, `score(X, y)`.
- **NN building blocks under nn.** Layers, optimizers, loss
  functions, parameter management, training loops. The PyTorch
  `nn.Module` shape fits here cleanly.
- **Geometry under linalg.** Quaternions, SE(3) / SO(3) transforms,
  rotation conversions, spatial vector types — where every shipping
  3D engine puts them.
- **Mixed precision is native, not an afterthought.** Tensors carry
  dtype; fp4 / fp6 / fp8 / fp16 / bf16 / fp32 / fp64 all participate.
  Casting between them is explicit and uses the boxed-primitive
  casting API.
- **GPU acceleration on day one — optional.** Kernels dispatch
  through [`cajeta.gpu`](gpu/CajetaGPU.md) when a device is
  present, fall back to CPU + LLVM SIMD intrinsics otherwise. Same
  `Tensor` API; the active backend resolves at runtime.

## Non-goals (v1)

- **Distributed training.** Multi-host / multi-GPU coordination
  belongs in a follow-up library that builds on `cajeta.math.nn`.
- **Pre-trained model zoo.** No bundled weights. The infrastructure
  supports loading from external sources (npy / safetensors / ONNX
  later); we don't curate a model library.
- **JIT graph compilers.** No XLA-equivalent in v1. Eager execution
  with vectorized kernels is the baseline; tracing / compilation is a
  later layer.
- **Probabilistic programming framework.** Distributions exist
  (`stats.distributions`); a Pyro / Stan-equivalent inference engine
  doesn't.

---

## Top-level `cajeta.math` — boxed primitives and friends

The foundation. Every native numeric type has an Object form here.
Higher layers (`tensor`, `linalg`, `stats`, `signal`, `nn`) reach into
these for casting, formatting, parsing, and inspection.

### Boxed primitives — full coverage

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
documented in `docs/stdlib/`) become aliases / re-exports of
`cajeta.math.Int32 / Int64 / Float64 / Boolean`. The math package is
the source of truth; `cajeta.lang` carries the Java-style names for
ergonomics.

### Common shape — every boxed numeric type

```cajeta
public final class Float8E4M3 implements Comparable<Float8E4M3> {
    // The wrapped value. Public so the unboxing path is a single
    // field access; the compiler can fold (heap Float8E4M3(x)).value
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
    public int8[1] rawBytes();                        // bit pattern as bytes
    public static Float8E4M3 fromRawBits(int8 bits);  // round-trip
    public static Float8E4M3 fromRawBytes(int8[1] b);

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

Same shape applies to every floating-point box — only the constants
and casting destinations differ. Integer boxes drop the IEEE
inspection methods, gain bit-manipulation helpers (`bitCount`,
`numberOfLeadingZeros`, `reverse`, etc.), and replace saturating
arithmetic with two-variant arithmetic (wrapping + checked).

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
RNG returned by `Random.default()`); it's the standard technique for
low-precision training to avoid systematic bias when quantizing
gradients and weights. The RNG dependency is the reason `Random`
lives at the top level of `cajeta.math` rather than nested under
`stats` — `RoundingMode` is in the boxed-primitive layer and can't
reach upward.

### Why fp4 / fp6 / fp8 deserve dedicated boxes

These formats have small dynamic range and tight quantization steps;
the failure modes (overflow to inf, underflow to zero, NaN
propagation, denormal flushing) bite immediately when used naively.
The boxed types make the gotchas inspectable (`isSubnormal()`,
`saturatingAdd`) and the casts to and from larger formats explicit.
Without that surface, fp8 training loops degrade silently in ways
that take days to debug.

The boxed types also give us the natural place to hang the conversion
correctness tests — a precision-down cast from `Float64` →
`Float8E4M3` has well-defined behavior we can pin down once and trust
everywhere.

### Arbitrary-precision types

```
cajeta.math.BigInteger     — arbitrary-precision signed integer
cajeta.math.BigDecimal     — arbitrary-precision decimal with scale
cajeta.math.Rational       — exact ratio of two BigIntegers
```

Standard arithmetic, comparisons, conversions to / from boxed
primitives (with explicit rounding when the destination can't
represent the value), parsing, formatting. Same shape as Java's
`BigInteger / BigDecimal` so users coming from JVM languages get a
familiar API.

### Numeric utilities

```
cajeta.math.Math       — sin, cos, exp, log, pow, sqrt, etc.
                         (ships today as cajeta.lang.Math; the
                         transcendentals and abs/min/max/sqrt/floor/
                         ceil/round are compiler intrinsics, so Math is
                         the documented surface over them — re-exported
                         here once cajeta.math lands)
cajeta.math.bit        — popcount, leading-zeros, byte-swap helpers
                         (currently scattered across Integer / Long;
                         consolidate)
cajeta.math.constants  — PI, E, TAU, GOLDEN, LN2, LN10, etc.
```

### Random number generation

A general-purpose RNG for stdlib-and-up code — counters, sampling,
shuffling, jitter, the everyday sources of stochasticity. Distinct
from `cajeta.math.stats.distributions` (tensor-shaped sampling) and
from `SecureRandom` (cryptographic). Mutable RNG object with
`nextX()` methods, the shape Java / C++ / most stdlibs converged on.

```cajeta
public final class Random {
    public Random();                              // seeded from entropy
    public Random(int64 seed);                    // reproducible

    // Algorithm choice via factory. Default = PCG64 (good
    // statistical quality, fast, small state); Xoshiro256** as
    // an alternative for callers wanting maximum throughput.
    public static Random pcg64(int64 seed);
    public static Random xoshiro256ss(int64 seed);

    // Primitives — return values across the type's full range.
    public boolean nextBoolean();
    public int8    nextInt8();
    public int16   nextInt16();
    public int32   nextInt32();
    public int64   nextInt64();
    public float32 nextFloat32();                 // [0, 1)
    public float64 nextFloat64();                 // [0, 1)

    // Bounded variants — rejection sampling so the distribution is
    // exactly uniform (not the modulo-bias trap).
    public int32 nextInt32(int32 boundExclusive);
    public int32 nextInt32(int32 lowInclusive, int32 highExclusive);
    public int64 nextInt64(int64 boundExclusive);
    public int64 nextInt64(int64 lowInclusive, int64 highExclusive);

    // Common distributions. Heavier sampling — multivariate, gamma,
    // beta, etc. — lives in cajeta.math.stats.distributions; this is
    // the everyday set.
    public float64 nextGaussian(float64 mean = 0.0, float64 stddev = 1.0);
    public float64 nextExponential(float64 lambda);

    // Bytes (filling a buffer in one shot is faster than per-byte).
    public void nextBytes(int8[] buffer);

    // In-place shuffle (Fisher-Yates).
    public <T> void shuffle(Array<T> arr);

    // State save / restore — for reproducible test failures.
    public int8[] saveState();
    public void   restoreState(int8[] state);

    // Process-global default. Per-fiber instance, lazily seeded
    // from the OS entropy source on first access, so concurrent
    // fibers don't contend on a shared RNG and don't produce
    // identical streams from a shared seed.
    public static Random default();
}
```

For cryptographic use:

```cajeta
public final class SecureRandom {
    // Always seeded from the OS entropy source; never accepts a
    // user seed. Backed by /dev/urandom on Linux, BCryptGenRandom
    // on Windows, SecRandomCopyBytes on macOS.
    public SecureRandom();

    // Same primitive surface as Random, minus the bounded
    // convenience helpers (callers needing crypto-grade ints
    // typically want raw bytes, not range-bounded scalars).
    public void    nextBytes(int8[] buffer);
    public int64   nextInt64();
    public float64 nextFloat64();
}
```

Algorithm pinning is intentional: the default RNG's algorithm is
documented and fixed across versions (PCG64 unless a future
specification calls out a replacement) so a saved seed produces the
same stream three years later.

### GUID — 32-bit, 64-bit, 128-bit globally unique identifiers

Three sizes covering the spectrum from "small ID with low collision
risk acceptable" through "snowflake-shaped sortable ID for high-
volume systems" through "standard UUID, effectively zero collision
risk."

```
cajeta.math.Guid32        — 4-byte globally unique identifier
cajeta.math.Guid64        — 8-byte globally unique identifier
cajeta.math.Guid128       — 16-byte UUID (RFC 4122 compatible)
```

#### `Guid32`

4 bytes. ~4.3 billion possible values; collision probability ≈ 50%
at ~77,000 generated IDs (birthday bound). Suitable for short-lived
IDs (request IDs, transient objects, log correlation), local
sequences augmented with a time epoch, or any scope where the
generating process can detect and recover from collision.

```cajeta
public final class Guid32 implements Comparable<Guid32> {
    public static Guid32 random();
    public static Guid32 random(Random rng);
    public static Guid32 timeOrdered();           // 22-bit seconds + 10-bit counter

    public static Guid32 of(int32 bits);
    public static Guid32 of(int8[4] bytes);

    public int32   bits();
    public int8[4] bytes();

    public String toString();                     // 8 lowercase hex digits
    public static Guid32 parse(String hex);

    public boolean operator==(Object obj);
    public int64   hash();
    public int32   compare(Guid32 other);
}
```

#### `Guid64`

8 bytes. ~1.8 × 10¹⁹ possible values; collision probability ≈ 50%
at ~5 billion. Snowflake-shape time-ordered variant suitable for
high-volume databases (sortable index, no UUID v4's pathological
b-tree fragmentation). Random variant available for cases where
ordering would leak information.

```cajeta
public final class Guid64 implements Comparable<Guid64> {
    public static Guid64 random();
    public static Guid64 random(Random rng);
    public static Guid64 snowflake();             // 41-bit ms + 13-bit node + 10-bit counter
    public static Guid64 snowflake(int16 nodeId);

    public static Guid64 of(int64 bits);
    public static Guid64 of(int8[8] bytes);

    public int64   bits();
    public int8[8] bytes();

    public String toString();                     // 16 lowercase hex digits
    public static Guid64 parse(String hex);

    public int64 timestampMillis();
    public int16 nodeId();
    public int16 counter();

    public boolean operator==(Object obj);
    public int64   hash();
    public int32   compare(Guid64 other);
}
```

#### `Guid128`

16 bytes. RFC 4122 UUID compatible. Effectively zero collision risk
for any realistic generation rate. The default for IDs that cross
system boundaries.

```cajeta
public final class Guid128 implements Comparable<Guid128> {
    public static Guid128 v4();                   // RFC 4122 v4 — 122 bits secure-random
    public static Guid128 v4(Random rng);
    public static Guid128 v7();                   // RFC 9562 v7 — sortable, ms timestamp

    public static Guid128 nil();
    public static Guid128 max();

    public static Guid128 of(int64 high, int64 low);
    public static Guid128 of(int8[16] bytes);

    public int64    high();
    public int64    low();
    public int8[16] bytes();

    public String toString();                     // "550e8400-e29b-41d4-a716-446655440000"
    public static Guid128 parse(String text);

    public int8 variant();                        // RFC 4122 = 1
    public int8 version();                        // 4 or 7

    public boolean operator==(Object obj);
    public int64   hash();
    public int32   compare(Guid128 other);
}
```

#### Choosing between sizes

| Use case                                            | Recommended  |
|-----------------------------------------------------|--------------|
| Short-lived request IDs, log correlation            | `Guid32`     |
| Database keys for high-volume tables                | `Guid64`     |
| Externally-visible identifiers, file references     | `Guid128.v4` |
| Database keys where time ordering improves indexes  | `Guid128.v7` |
| Session tokens, secrets                             | `Guid128.v4` |

The explicit size in the type name makes the trade-off visible at
every read site instead of buried in choice of generator.

---

## Sub-package map

```
cajeta.math.tensor   ── N-d arrays + the dtype/shape/strides machinery
                       (the NumPy core). Also npy / npz / safetensors IO.

cajeta.math.linalg   ── matmul, decompositions (LU, QR, SVD, Cholesky,
                       eigenvalues), solvers, norms, pseudoinverse. The
                       geometry types live here too — Vec2/3/4, Mat2/3/4,
                       Quaternion, SE(3) / SO(3), since they're linear
                       algebra by construction.

cajeta.math.stats    ── descriptive (mean, std, var, quantiles, skew,
                       kurtosis), distributions (pdf / cdf / ppf,
                       multivariate, mixtures), hypothesis tests
                       (t, chi-sq, KS, Mann-Whitney, ANOVA), correlation,
                       EWMA, AND the sklearn-shaped classical-ML
                       estimators (regression, classification,
                       clustering, trees, preprocessing, metrics).

cajeta.math.signal   ── FFT (1-D / 2-D / N-D, real, DCT), convolution,
                       correlation, filter design (FIR / IIR), windowing,
                       resampling, spectrograms.

cajeta.math.nn       ── autograd (Variable wrapper, dynamic graphs,
                       no-grad scopes), layer primitives (Linear, Conv,
                       norms, attention, transformers), losses,
                       optimizers (SGD, Adam, AdamW, LAMB, ...), LR
                       schedulers, generic gradient-free optimization
                       (BFGS, L-BFGS, Nelder-Mead, simulated annealing,
                       differential evolution).
```

Five sub-packages. Each is self-contained on top of the boxed-
primitive foundation; cross-package dependencies are explicit (`nn`
depends on `tensor` and `linalg`; `stats` depends on `tensor` and
`linalg`; `signal` depends on `tensor`).

---

## `cajeta.math.tensor`

The foundational type. Everything else in the sub-packages flows
through `Tensor`.

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
    public int64       count();       // total element count
    public int8        rank();        // shape.rank()

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

    public int8    bytesPerElement();
    public boolean isFloat();
    public boolean isInteger();
    public boolean isComplex();
    public boolean isSigned();
}
```

Implementation strategy: storage is a single heap buffer (`int8[]`)
sized to `dtype.bytesPerElement() * shape.totalSize()`, plus a small
header for shape / strides / offset. Views (slice / reshape /
transpose) share the underlying buffer; only `astype` and explicit
copies allocate.

`Tensor<T>` parameterizing on the element type (`int32`, `float32`,
etc., not the boxed forms) lets the compiler specialize hot kernels
per dtype. The `DType` runtime tag covers the generic-over-dtype API
surface (`astype`, IO).

### Tensor IO (`cajeta.math.tensor.io`)

Same sub-package, separate module:

- **NumPy `.npy`** — single-array, fully reversible.
- **NumPy `.npz`** — multi-array zip container.
- **safetensors** — modern alternative used by HuggingFace; safer than
  pickled `.pt` since it can't execute arbitrary code on load.
- **CSV** — simple float / int loaders that produce 2-D tensors.
- **Image loaders** — PNG / JPEG / EXR to / from `Tensor<uint8>` or
  `Tensor<float32>`; lands later, behind a feature flag.

ONNX import / export lives in `cajeta.math.nn.onnx` because it's
graph-shaped, not tensor-shaped.

---

## `cajeta.math.linalg`

Linear algebra. Matrix operations, decompositions, solvers — plus the
geometry types (Vec*, Mat*, Quaternion, SE(3)) that are linear algebra
by construction.

### Core surface

```cajeta
// Matrix multiplication. Broadcasts over batch dimensions; final two
// dims are (M, K) x (K, N) -> (M, N). Dispatches to vendor BLAS via
// cajeta.gpu.blas when an XPU device is present.
public Tensor<T> matmul<T>(Tensor<T> a, Tensor<T> b);

// Decompositions — all return Tensor tuples / structured types.
public LU<T>       lu<T>(Tensor<T> a);          // PA = LU
public QR<T>       qr<T>(Tensor<T> a);          // A = QR
public SVD<T>      svd<T>(Tensor<T> a);         // A = U Σ V^T
public Cholesky<T> cholesky<T>(Tensor<T> a);    // SPD: A = L L^T
public Eigen<T>    eigvals<T>(Tensor<T> a);

// Solvers
public Tensor<T> solve<T>(Tensor<T> A, Tensor<T> b);          // Ax = b
public Tensor<T> lstsq<T>(Tensor<T> A, Tensor<T> b);          // least-squares
public Tensor<T> inv<T>(Tensor<T> A);
public Tensor<T> pinv<T>(Tensor<T> A);                        // pseudoinverse

// Norms and matrix utilities
public T         norm<T>(Tensor<T> a, NormKind kind = NormKind.FROBENIUS);
public T         det<T>(Tensor<T> A);
public T         trace<T>(Tensor<T> A);
public int64     matrixRank<T>(Tensor<T> A, T tol = T(0));
```

### Geometry

```cajeta
public final class Vec2<T> { public T x; public T y; /* arithmetic, dot, length, normalize */ }
public final class Vec3<T> { public T x; public T y; public T z; /* + cross */ }
public final class Vec4<T> { public T x; public T y; public T z; public T w; }

public final class Mat3<T> { /* 3x3 */ }
public final class Mat4<T> { /* 4x4 — homogeneous transforms */ }

public final class Quaternion<T> {
    public T w; public T x; public T y; public T z;

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

public final class DualQuaternion<T> {
    public Quaternion<T> real;
    public Quaternion<T> dual;
    /* compose, inverse, slerp, conversion to/from Mat4 — skinning workhorse */
}

public final class SE3<T> {
    public Mat3<T> rotation;        // SO(3)
    public Vec3<T> translation;
    /* compose, inverse, applyTo(Vec3), applyTo(SE3), interpolate, ... */
}

public final class SO3<T> {
    public Mat3<T> rotation;
    /* expmap, logmap, geodesic interpolation, ... */
}
```

Generic over `T` so `Quaternion<float32>` (graphics) and
`Quaternion<float64>` (control, robotics) both work without a second
implementation. The graphics layer (`cajeta.render`) imports these for
mesh transforms, camera math, and skeletal animation; physics, IK,
and SLAM workloads use the same types.

### Backend dispatch

Linalg kernels dispatch through `cajeta.gpu.blas` /
`cajeta.gpu.dnn` when an accelerator is present and the operation
is large enough to amortize the launch overhead. Below that threshold,
or when no XPU device is configured, the CPU fallback uses LLVM SIMD
intrinsics — see §"Backend strategy" for the full story.

---

## `cajeta.math.stats`

Descriptive and inferential statistics, distributions, hypothesis
tests, AND the sklearn-shaped classical-ML estimators. The unifying
shape: all of it is statistical modeling — descriptive stats are
zero-parameter models, regression is parametric inference, clustering
is unsupervised inference. Living together under `stats` matches that
reality and frees `nn` to be neural-network-specific.

### Descriptive statistics

```cajeta
public T   mean<T>(Tensor<T> x, int8... axes);
public T   median<T>(Tensor<T> x, int8... axes);
public T   var<T>(Tensor<T> x, int32 ddof = 1, int8... axes);
public T   std<T>(Tensor<T> x, int32 ddof = 1, int8... axes);
public T   quantile<T>(Tensor<T> x, T q, int8... axes);
public T   skew<T>(Tensor<T> x, int8... axes);
public T   kurtosis<T>(Tensor<T> x, int8... axes);
public T   corr<T>(Tensor<T> x, Tensor<T> y);             // Pearson
public T   spearman<T>(Tensor<T> x, Tensor<T> y);

public EWMA<T> ewma<T>(Tensor<T> x, T alpha);              // exp-weighted moving avg
```

### Distributions

```cajeta
public abstract class Distribution<T> {
    public abstract T       pdf(T x);
    public abstract T       cdf(T x);
    public abstract T       ppf(T q);                      // quantile / inverse cdf
    public abstract T       sample(Random rng);
    public abstract Tensor<T> sample(Random rng, Shape shape);
    public abstract T       mean();
    public abstract T       variance();
}

public class Normal<T>      extends Distribution<T> { ... }
public class Uniform<T>     extends Distribution<T> { ... }
public class Gamma<T>       extends Distribution<T> { ... }
public class Beta<T>        extends Distribution<T> { ... }
public class Exponential<T> extends Distribution<T> { ... }
public class Poisson        extends Distribution<int64> { ... }
public class Binomial       extends Distribution<int64> { ... }
public class Categorical    extends Distribution<int32> { ... }
public class Dirichlet<T>   extends MultivariateDistribution<T> { ... }
public class MultivariateNormal<T> extends MultivariateDistribution<T> { ... }
public class GaussianMixture<T>    extends MultivariateDistribution<T> { ... }
```

### Hypothesis tests

```cajeta
public TestResult tTest(Tensor<float64> a, Tensor<float64> b, TTestKind kind = TTestKind.WELCH);
public TestResult chiSqGoodnessOfFit(Tensor<int64> observed, Tensor<float64> expected);
public TestResult chiSqIndependence(Tensor<int64> contingency);
public TestResult ksTest(Tensor<float64> a, Tensor<float64> b);
public TestResult mannWhitneyU(Tensor<float64> a, Tensor<float64> b);
public TestResult anovaOneWay(Array<Tensor<float64>> groups);
public TestResult shapiroWilk(Tensor<float64> a);
```

### Classical ML — sklearn estimator contract

```cajeta
public interface Estimator<X, Y> {
    public Self    fit(X data, Y target);
    public Y       predict(X data);
    public float64 score(X data, Y target);
}
```

Regression:

```cajeta
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

Classification: `KNN`, `SVM` (linear / kernel), `NaiveBayesGaussian` /
`NaiveBayesMultinomial` / `NaiveBayesBernoulli`, `LDA` / `QDA`.

Clustering: `KMeans` (with k-means++ init), `MiniBatchKMeans`,
`HierarchicalClustering` (single / complete / average / Ward),
`DBSCAN`, `OPTICS`, `GaussianMixture` (under both clustering and
distributions — same model, two entry points).

Trees: `DecisionTreeClassifier`, `DecisionTreeRegressor`,
`RandomForestClassifier`, `RandomForestRegressor`, `ExtraTreesClassifier`,
`GradientBoostingClassifier`, `GradientBoostingRegressor`.

Preprocessing: `StandardScaler`, `MinMaxScaler`, `RobustScaler`,
`OneHotEncoder`, `OrdinalEncoder`, `SimpleImputer`, `PCA`, `TSNE`,
`UMAP`.

Metrics: `accuracy`, `precision`, `recall`, `f1`, `auc`, `mse`, `mae`,
`r2`, `logLoss`, `confusionMatrix`, `classificationReport`.

---

## `cajeta.math.signal`

Signal processing — what NumPy splits across `numpy.fft` and `scipy.signal`.
Living together makes sense: nearly every signal-processing pipeline
moves between the time and frequency domains.

### FFT

```cajeta
public Tensor<complex64>  fft<T>(Tensor<T> x, int8 axis = -1);
public Tensor<complex64>  ifft<T>(Tensor<complex64> X, int8 axis = -1);
public Tensor<complex64>  fft2<T>(Tensor<T> x);
public Tensor<complex64>  ifft2<T>(Tensor<complex64> X);
public Tensor<complex64>  fftn<T>(Tensor<T> x, int8... axes);
public Tensor<complex64>  ifftn<T>(Tensor<complex64> X, int8... axes);

// Real-input variants (cheaper)
public Tensor<complex64>  rfft<T>(Tensor<T> x, int8 axis = -1);
public Tensor<T>          irfft<T>(Tensor<complex64> X, int64 n, int8 axis = -1);

// DCT / DST
public Tensor<T>          dct<T>(Tensor<T> x, DCTType type = DCTType.II);
public Tensor<T>          idct<T>(Tensor<T> X, DCTType type = DCTType.II);
```

Dispatches to `cajeta.gpu.fft` (cuFFT / rocFFT) when an XPU
device is present and the transform is large enough; FFTW-shaped CPU
plans below that threshold.

### Convolution and correlation

```cajeta
public Tensor<T> convolve<T>(Tensor<T> x, Tensor<T> kernel, ConvMode mode = ConvMode.FULL);
public Tensor<T> correlate<T>(Tensor<T> x, Tensor<T> kernel, ConvMode mode = ConvMode.FULL);

// 2-D
public Tensor<T> convolve2d<T>(Tensor<T> x, Tensor<T> kernel, ConvMode mode = ConvMode.FULL);
public Tensor<T> correlate2d<T>(Tensor<T> x, Tensor<T> kernel, ConvMode mode = ConvMode.FULL);
```

### Filter design

```cajeta
public FIRFilter<T>  firDesignWindow<T>(int32 order, T cutoffNormalized, WindowKind window = WindowKind.HAMMING);
public IIRFilter<T>  butterworth<T>(int32 order, T cutoffNormalized, BandKind band = BandKind.LOWPASS);
public IIRFilter<T>  chebyshev1<T>(int32 order, T ripple, T cutoffNormalized, BandKind band);
public IIRFilter<T>  chebyshev2<T>(int32 order, T attenuation, T cutoffNormalized, BandKind band);
public IIRFilter<T>  elliptic<T>(int32 order, T ripple, T attenuation, T cutoffNormalized, BandKind band);
public IIRFilter<T>  bessel<T>(int32 order, T cutoffNormalized, BandKind band);

public Tensor<T>     applyFilter<T>(FIRFilter<T> filt, Tensor<T> x);
public Tensor<T>     applyFilter<T>(IIRFilter<T> filt, Tensor<T> x);
```

### Windowing, resampling, spectrograms

```cajeta
public Tensor<T> window<T>(int64 n, WindowKind kind);                // hamming, hann, blackman, kaiser, ...
public Tensor<T> resample<T>(Tensor<T> x, int64 numNewSamples);      // polyphase
public Tensor<T> resamplePoly<T>(Tensor<T> x, int32 up, int32 down);

public Tensor<T> spectrogram<T>(Tensor<T> x, int32 nperseg = 256, int32 noverlap = 128, WindowKind window = WindowKind.HANN);
public Tensor<T> stft<T>(Tensor<T> x, int32 nperseg, int32 noverlap, WindowKind window);
public Tensor<T> istft<T>(Tensor<complex64> Z, int32 nperseg, int32 noverlap, WindowKind window);
```

---

## `cajeta.math.nn`

Autograd plus everything that depends on it: layers, optimizers, loss
functions, training loops. Plus the gradient-free generic optimizers
(BFGS family, Nelder-Mead, simulated annealing) — same package because
they share the "optimize a parameter set against an objective" shape,
even when no gradient is involved.

### Autograd

PyTorch-style dynamic graphs. A `Variable` carries data, a gradient
buffer, and a `gradFn` reference; operations build the graph
implicitly; `backward()` walks it in reverse.

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
softmax, log, sum, mean, ...) ship as built-ins; each delegates its
forward pass to `cajeta.math.tensor` (and through it to
`cajeta.gpu` when an accelerator is present).

### `Module` base + parameter discovery

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
```

### Layer set for v1

`Linear`, `Conv1d`, `Conv2d`, `Conv3d`, `BatchNorm{1,2,3}d`,
`LayerNorm`, `RMSNorm`, `Dropout`, `Embedding`, `MultiheadAttention`,
`TransformerEncoderLayer`, `TransformerDecoderLayer`.

Activations as Module-or-function pairs: `ReLU`, `GELU`, `SiLU`,
`Tanh`, `Sigmoid`, `Softmax`.

Losses: `MSELoss`, `CrossEntropyLoss`, `BCELoss`, `L1Loss`,
`SmoothL1Loss`, `HuberLoss`, `KLDivLoss`.

```cajeta
public class Linear extends Module {
    public Variable<float32> weight;
    public Variable<float32> bias;

    public Linear(int64 inFeatures, int64 outFeatures, boolean bias = true);
    public Variable<float32> forward(Variable<float32> x);
}
```

### Optimizers

```cajeta
public abstract class Optimizer {
    public abstract void step();
    public abstract void zeroGrad();
}

public class SGD     extends Optimizer { /* momentum, Nesterov */ }
public class Adam    extends Optimizer { /* β1, β2, ε, weight decay */ }
public class AdamW   extends Optimizer { /* decoupled weight decay */ }
public class RMSProp extends Optimizer { /* */ }
public class Adagrad extends Optimizer { /* */ }
public class LAMB    extends Optimizer { /* large-batch */ }
```

LR schedulers: `StepLR`, `MultiStepLR`, `ExponentialLR`,
`CosineAnnealingLR`, `OneCycleLR`, `ReduceLROnPlateau`.

### Gradient-free optimization

For users whose objective doesn't come with gradients (black-box
functions, simulation-based scoring, integer or constrained spaces):

```cajeta
public OptimResult bfgs<T>(    (Tensor<T>) -> T fn, Tensor<T> x0);
public OptimResult lbfgs<T>(   (Tensor<T>) -> T fn, Tensor<T> x0, int32 memorySize = 10);
public OptimResult nelderMead<T>((Tensor<T>) -> T fn, Tensor<T> x0);
public OptimResult simulatedAnnealing<T>((Tensor<T>) -> T fn, Tensor<T> x0, SAConfig cfg);
public OptimResult differentialEvolution<T>((Tensor<T>) -> T fn, Tensor<T> bounds, DEConfig cfg);
```

BFGS / L-BFGS internally use gradient estimation (finite differences
by default, or a user-supplied gradient function). They share the
infrastructure with autograd-based optimizers — `OptimResult` is the
same type either way.

### ONNX import/export

`cajeta.math.nn.onnx`:

```cajeta
public Module loadONNX(String path);
public void   saveONNX(Module model, Tensor<float32> exampleInput, String path);
```

ONNX lives under `nn` (rather than under `tensor.io`) because the
import / export operates on `Module` graphs, not raw tensors.

---

## Backend strategy

The same `Tensor` / `Module` API targets one portable accelerator
surface — `cajeta.gpu` — which the compiler lowers to four
backends from one source:

- **CPU + LLVM SIMD intrinsics** — the default and the only one with
  no external dependencies, and the floor `cajeta.gpu` always
  lowers to when no GPU is present. Loop kernels are aggressively
  autovectorized; AVX-512, NEON, SVE are all in scope.
- **Vulkan (SPIR-V)** — portable cross-vendor compute (Intel Arc,
  Apple via MoltenVK, mobile, lavapipe). Linalg uses cooperative-matrix
  kernels.
- **AMD (ROCm / AMDGPU)** — when a HIP device is present.
- **NVIDIA (NVPTX)** — when a CUDA device is present.

`cajeta.gpu` is the *only* GPU package in the stdlib: there are no
per-vendor `cajeta.gpu.nvidia` / `.amd` / `.vulkan` stdlib packages.
Vendor-peak library bindings (cuBLAS / cuDNN / cuFFT, rocBLAS / MIOpen /
rocFFT) and vendor-exclusive silicon paths live in **external vendor
libraries** layered under the same `cajeta.gpu.{blas,dnn,fft}`
seams — added to a project explicitly, never bundled in stdlib. See
[`CajetaGPU.md`](gpu/CajetaGPU.md) for the foundation model.

The active backend is selected at runtime on the first device touch
(`CUDA → HIP → Vulkan → CPU`); the API doesn't change. A program written
against `cajeta.math.linalg.matmul(a, b)` runs on whichever backend is
present, with no source-level branching.

When no GPU is present the math sub-packages still build and run — they
just stay on CPU. The GPU dependency is genuinely optional; dropping
`cajeta.gpu` from `Cajeta.toml` strips the device-dispatch path out
of the compiled library entirely.

For the substrate that makes accelerator dispatch possible — the device
foundation, value types, buffers, kernels, and per-backend lowering —
see [`CajetaGPU.md`](gpu/CajetaGPU.md) and the compute layer
[`CajetaXPU.md`](CajetaXPU.md).

---

## Implementation sequence

A reasonable order, given dependencies:

1. **Boxed primitives + `RoundingMode` + casting API.** No sub-package
   can land cleanly without these. The intrinsic-fp tests already in
   the suite (`Fp*Tests`) cover the underlying lowering; this step
   adds the user-visible Object API.
2. **BigInteger, BigDecimal, Rational.** Standalone, useful outside
   the math library, lets later steps reuse the arbitrary-precision
   path (exact statistical computations).
3. **`Random`, `SecureRandom`, `Guid{32,64,128}`.** Top-level under
   `cajeta.math`. Self-contained, unblocks tensor sampling and
   identifier-generation in apps.
4. **`cajeta.math.tensor`.** The foundation. Strided storage,
   broadcasting, slicing, reductions, the dtype tag. Element-wise ops
   as a baseline (no autograd yet, no linalg yet). Plus npy / npz IO.
5. **`cajeta.math.linalg`.** matmul + basic decompositions + the
   geometry types. Wraps reference-implementation kernels initially;
   LAPACK / OpenBLAS / `cajeta.gpu.blas` plug in later behind
   the same surface.
6. **`cajeta.math.stats` — descriptive + distributions.** Self-
   contained block atop `tensor` + `linalg`. Hypothesis tests next.
7. **`cajeta.math.stats` — classical ML.** Regression, classification,
   clustering, trees, preprocessing, metrics. Sklearn-shaped
   estimators. Each is independent; ship as you go.
8. **`cajeta.math.signal` — FFT.** Stand-alone block.
9. **`cajeta.math.signal` — convolution, correlation, filter design.**
10. **`cajeta.math.nn` — autograd.** The Variable wrapper, backward
    graph, function registration. Built on `tensor`.
11. **`cajeta.math.nn` — layers, optimizers, training loop.**
    Transformer block early so language-model workloads are reachable.
12. **`cajeta.math.nn` — gradient-free optimizers.** BFGS / L-BFGS,
    Nelder-Mead, SA, DE. Independent of the autograd path.
13. **Accelerator wiring through `cajeta.gpu.{blas,dnn,fft}`.**
    Backends light up under the same API. The CPU path keeps working.

The gating step is (1) — every other layer touches boxed numerics,
casting, and rounding. It's small in surface area but foundational.

---

## Open questions

- **Boxed primitive locations.** Keep `cajeta.lang.Integer / Long /
  Double` as the everyday names and treat `cajeta.math.Int32 /
  Int64 / Float64` as canonical, or fold the everyday names into
  `cajeta.math` directly? Java's split has historical baggage; we
  could pick the cleaner shape.
- **DType representation.** Runtime-tag (`DType`) on a tensor generic
  over `T` produces a small inconsistency: `Tensor<float32>` knows its
  element type at compile time, but `astype` returns a tensor with the
  runtime-chosen dtype. Worth resolving — probably with a separate
  `AnyTensor` base type that erases the element type for the dtype-
  generic API.
- **Stochastic rounding RNG sourcing.** `RoundingMode.STOCHASTIC`
  needs an RNG; current sketch reaches into `Random.default()`.
  Alternative: stochastic rounding API takes an explicit RNG argument
  and isn't a `RoundingMode` enum value at all — it's a runtime-only
  mode that can't be requested via simple casts. Worth a decision
  before locking the API.
- **Autograd graph storage.** Eager build (every op records into the
  current graph) vs. trace-on-demand (record only inside an explicit
  `withGrad { }` scope). Eager matches PyTorch and is intuitive;
  trace-on-demand is cheaper when most code is forward-only. Defer
  until autograd implementation starts.
- **Module parameter discovery.** Synthesize via `@Trainable`
  annotation walked by the compiler, or by reflection at runtime?
  Compiler-walked is faster and statically-checked; runtime is
  flexible and matches the Python ergonomics ML researchers expect.
  Worth picking a side once `nn.Module` lands.
- **Where does GUID actually belong?** `cajeta.math.Guid{32,64,128}`
  is the current sketch — math because the random / time-ordered
  generation uses RNG and timestamps. But GUIDs aren't numerical
  per se; an argument could be made for `cajeta.lang.Guid*` or
  `cajeta.util.Guid*`. Worth revisiting after the rest of the math
  sub-packages settle.
