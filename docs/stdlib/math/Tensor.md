# Tensor\<T\>

`cajeta.math.Tensor` — the keystone n-dimensional array: a strided view over
a shared, refcounted `Storage` (`offset` + `shape` + `strides`). The element
type is the static, reified type parameter `T` (the dtype lives in `T`; see
[DType](DType.md)); rank and shape are runtime values, so `reshape` and
variable dims are expressible — numpy's model. Factories return an owned
tensor; views (`alias`, `slice`, `transpose`, …) share the buffer by
refcount, and it frees only when the last drops. All tensors are C-order
(row-major).

The construction statics are method-templated — call them with an explicit
element type:

```cajeta
package snip.tensor;

import cajeta.math.Tensor;

public final class Demo {
    public static void run() {
        int64[] shape = heap int64[2];
        shape[0] = 2;
        shape[1] = 3;
        Tensor<float32> a #= Tensor.zeros<float32>(shape);   // 2x3, C-order
        a.set2(0, 2, 7.0f);
        Tensor<float32> b #= Tensor.ones<float32>(shape);
        Tensor<float32> sum #= Tensor.add<float32>(a, b);    // elementwise
        Tensor<float32> t #= sum.transpose();                // 3x2 view
        float32 total = Tensor.sum<float32, float32>(sum);
        return;
    }
}
```

## Methods

### Construction

| Signature | |
|---|---|
| `Tensor(Storage<T> store, int64 offset, int64[] shape, int64[] strides)` | Wrap a `Storage` with an offset + shape + strides |
| `static int64 productOf(int64[] shape)` | Product of the dims — the element count of a tensor with this shape |
| `static #int64[] contigStrides(int64[] shape)` | Contiguous strides (in elements) for `shape`: C-order |
| `static #Tensor<E> empty<E>(int64[] shape)` | Uninitialized tensor of `shape` (C-order) |
| `static #Tensor<E> zeros<E>(int64[] shape)` ⚑ | Tensor of `shape` filled with the element zero |
| `static #Tensor<E> ones<E>(int64[] shape)` ⚑ | Tensor of `shape` filled with the element one |
| `static #Tensor<E> full<E>(int64[] shape, E value)` | Tensor of `shape` filled with `value` |
| `static #Tensor<E> arange<E>(int64 n)` ⚑ | 1-D tensor `[0, 1, …, n-1]` cast to `E` |
| `static #Tensor<E> arange<E>(E start, E stop, E step)` | 1-D tensor over the half-open interval `[start, stop)` in steps of `step` |
| `static #Tensor<E> of<E>(E[] data, int64[] shape)` ⚑ | Tensor of `shape` (C-order) holding a copy of `data` |
| `static #int64[] shapeOf<E>(Tensor<E> src)` | A copy of `src`'s shape vector |
| `static #Tensor<E> zerosLike<E>(Tensor<E> src)` | Zeros with `src`'s shape |
| `static #Tensor<E> onesLike<E>(Tensor<E> src)` | Ones with `src`'s shape |
| `static #Tensor<E> fullLike<E>(Tensor<E> src, E value)` | `value` fill with `src`'s shape |
| `static #Tensor<E> linspace<E>(E start, E stop, int64 num)` | `num` evenly spaced samples over the closed interval `[start, stop]` (numpy `linspace`) |
| `static #Tensor<E> eye<E>(int64 n)` | `n` x `n` identity matrix (numpy `eye(n)`) |
| `static #Tensor<E>[] meshgrid<E>(Tensor<E> x, Tensor<E> y)` | Coordinate grids from two 1-D vectors (numpy `meshgrid`, 'xy' indexing) |

### Shape and metadata

| Signature | |
|---|---|
| `int32 ndim()` | Number of dimensions |
| `int64 size()` | Total element count (product of the shape) |
| `int64 shapeAt(int32 axis)` | Extent of axis `axis` |
| `int64 strideAt(int32 axis)` | Stride (in elements) of axis `axis` |
| `int64 offset()` | Element offset of this tensor into its `Storage` |
| `#int64[] shape()` | A fresh, owned copy of the shape vector |
| `#DType dtype()` | The dtype descriptor of `T` (reified, via `DType.of`) |
| `int32 itemsize()` | Bytes per element |
| `int64 nbytes()` | Total bytes of the elements (`size * itemsize`) |
| `boolean isView()` | `true` if this is a view sharing another tensor's `Storage` |
| `Tensor<T> base()` | The tensor this view shares storage with, or `null` if it owns its storage |
| `boolean isContiguous()` | `true` if the strides are C-contiguous (row-major, no gaps) |

### Element access

| Signature | |
|---|---|
| `T getAt(int64[] idx)` | Read the element at multi-index `idx` (length must equal `ndim`) |
| `void setAt(int64[] idx, T v)` | Write `v` to the element at multi-index `idx` |
| `T get1(int64 i)` | 1-D element read |
| `void set1(int64 i, T v)` | 1-D element write |
| `T get2(int64 r, int64 c)` | 2-D element read |
| `void set2(int64 r, int64 c, T v)` | 2-D element write |
| `T flatGet(int64 i)` | Linear (flat) read by storage index — for a contiguous tensor |
| `void flatSet(int64 i, T v)` | Linear (flat) write — contiguous-tensor companion of `flatGet` |

### Device placement

| Signature | |
|---|---|
| `void gpu()` | Mirror this tensor's storage onto the device (eager) |
| `void cpu()` | Bring this tensor's storage back to the host |
| `boolean isOnGpu()` | `true` when the storage currently resides on the device |
| `int32 device()` | Placement code: `0` = host (CPU), `1` = device (GPU) |
| `KernelBuffer<T> deviceBuffer()` | The device buffer backing this tensor (null until `gpu()`) — the handle a `cajeta.xpu` kernel launch binds |

### Interop (TensorProtocol)

| Signature | |
|---|---|
| `#TensorProtocol protocol()` | Export through the `TensorProtocol` interop seam: a dtype-erased description borrowing this tensor's `Storage` |
| `static #Tensor<?> fromProtocol(TensorProtocol p)` | Import a `TensorProtocol` as a `Tensor<?>` |
| `static #Tensor<?> fromProtocolContiguous(TensorProtocol p)` | Import, materializing a contiguous copy |

### Views and reshaping

| Signature | |
|---|---|
| `#Tensor<T> alias()` | A whole-array view sharing this tensor's `Storage` (no copy) |
| `#Tensor<T> copy()` | An independent, contiguous (C-order) copy — own `Storage`, not a view |
| `#Tensor<T> reshape(int64[] newShape)` | Same data, new shape (same element count) |
| `#Tensor<T> transpose()` | Reverse the axes (full transpose): a view with shape and strides reversed |
| `#Tensor<T> slice(int32 axis, int64 start, int64 stop)` | A view of the half-open range `[start, stop)` along `axis` (step 1) |
| `#Tensor<T> squeeze()` | A view with every size-1 axis removed |
| `#Tensor<T> expandDims(int32 axis)` | A view with a new size-1 axis inserted at `axis` |
| `static #int64[] broadcastShape<E>(int64[] a, int64[] b)` | The broadcast result-shape of `a` and `b` under the standard right-aligned rule |
| `#Tensor<T> broadcastTo(int64[] targetShape)` | A zero-copy view stretched to `targetShape` — stretched axes get stride 0 |
| `#Tensor<T> ravel()` | 1-D flattening (numpy `ravel`): a view when contiguous, otherwise a fresh copy |
| `#Tensor<T> flatten()` | 1-D copy (numpy `flatten`): always independent contiguous storage |
| `#Tensor<T> swapaxes(int32 a, int32 b)` | Swap axes `a` and `b` (numpy `swapaxes`): a view |
| `#Tensor<T> moveaxis(int32 src, int32 dst)` | Move axis `src` to position `dst` (numpy `moveaxis`): a view |
| `#Tensor<T> transposeAxes(int32[] perm)` | General axis permutation (numpy `transpose(axes)`): a view |
| `#Tensor<T> flipAll()` | Reverse element order along every axis (numpy `flip` with `axis=None`): a view |

### Joining, splitting and layout ops

| Signature | |
|---|---|
| `static #Tensor<E> concatenate<E>(Tensor<E>[] parts, int32 axis)` | Join `parts` along an existing `axis` (numpy `concatenate`) |
| `static #Tensor<E> stackTensors<E>(Tensor<E>[] parts, int32 axis)` | Join equal-shaped `parts` along a new `axis` (numpy `stack`) |
| `static #Tensor<E>[] split<E>(Tensor<E> t, int32 nparts, int32 axis)` | Split into `nparts` equal sections along `axis` (numpy `split`); independent copies |
| `static #Tensor<E> tile<E>(Tensor<E> t, int64[] reps)` | Tile by `reps[i]` along each axis (numpy `tile`) |
| `static #Tensor<E> repeat<E>(Tensor<E> t, int32 n, int32 axis)` | Repeat each element `n` times along `axis` (numpy `repeat`) |
| `static #Tensor<E> pad<E>(Tensor<E> t, int64[] before, int64[] after, E value)` | Constant-pad each axis by `before[i]`/`after[i]` with `value` (numpy `pad`, mode='constant') |
| `static #Tensor<E> roll<E>(Tensor<E> t, int64 shift, int32 axis)` | Cyclically shift elements by `shift` along `axis` (numpy `roll`) |
| `static #Tensor<E> diagonal<E>(Tensor<E> t, int64 offset)` | The `offset`-th diagonal of a 2-D tensor as a 1-D copy (numpy `diagonal`) |
| `static #Tensor<E> tril<E extends Numeric>(Tensor<E> t, int64 k)` | Lower triangle of a 2-D tensor (numpy `tril`): `col <= row + k` kept, rest zeroed |
| `static #Tensor<E> triu<E extends Numeric>(Tensor<E> t, int64 k)` | Upper triangle (numpy `triu`): `col >= row + k` kept, rest zeroed |
| `static #Tensor<E> compress<E>(Tensor<E> t, Tensor<boolean> cond, int32 axis)` | Keep the slices along `axis` where the 1-D boolean `cond` is true (numpy `compress`) |
| `static #Tensor<E> choose<E>(Tensor<int64> indices, Tensor<E>[] choices)` | Pick each element from one of `choices` by an `indices` tensor (numpy `choose`) |

### Linear algebra

| Signature | |
|---|---|
| `static #Tensor<E> matmul<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | 2-D matrix product `(m,k)·(k,n) → (m,n)` (numpy `matmul`) |
| `static E dot<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | 1-D inner product `Σ a[i] * b[i]` as a scalar (numpy `dot` of two vectors) |
| `static E vdot<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Inner product over the flattened elements of two equal-shaped tensors (numpy `vdot`, real case) |
| `static #Tensor<E> outer<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Outer product of two 1-D vectors → 2-D (numpy `outer`) |
| `static E trace<E extends Numeric>(Tensor<E> a, int64 offset)` | Sum of the `offset`-th diagonal of a 2-D tensor (numpy `trace`) |
| `static #Tensor<E> kron<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Kronecker product of two 2-D tensors (numpy `kron`) |
| `static #Tensor<E> matrixPower<E extends Numeric>(Tensor<E> a, int32 n)` | Integer matrix power of a square 2-D tensor (numpy `matrix_power`, `n >= 0`) |
| `static #Tensor<E> tensordot<E extends Numeric>(Tensor<E> a, Tensor<E> b, int32 naxes)` | Contraction over the last `naxes` of `a` and the first `naxes` of `b` (numpy `tensordot`) |
| `static #Tensor<E> inner<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Inner product over the last axis of each operand (numpy `inner`) |
| `static #Tensor<E> einsum<E extends Numeric>(String spec, Tensor<E>[] operands)` | Einstein-summation contraction (numpy `einsum`) over an explicit subscript `spec` |

### Sorting and searching

| Signature | |
|---|---|
| `static #Tensor<E> sort<E extends Numeric>(Tensor<E> t, int32 axis)` | Sorted copy along `axis`, ascending (numpy `sort`) |
| `static #Tensor<int64> argsort<E extends Numeric>(Tensor<E> t, int32 axis)` | Index permutation that sorts along `axis` (numpy `argsort`) |
| `static #Tensor<int64> searchsorted<E extends Numeric>(Tensor<E> sorted, Tensor<E> values, int32 side)` | Insertion indices of `values` into the sorted 1-D `sorted` (numpy `searchsorted`) |
| `static #Tensor<E> partition<E extends Numeric>(Tensor<E> t, int64 kth, int32 axis)` | Partitioned copy about the `kth` order statistic (numpy `partition`) |
| `static #Tensor<int64> argpartition<E extends Numeric>(Tensor<E> t, int64 kth, int32 axis)` | Index permutation that partitions about the `kth` order statistic (numpy `argpartition`) |
| `static #Tensor<E> unique<E extends Numeric>(Tensor<E> t)` | Sorted distinct values over the flattened input (numpy `unique`) |
| `static #Tensor<int64> flatnonzero<E extends Numeric>(Tensor<E> t)` | C-order flat indices of the nonzero elements (numpy `flatnonzero`) |
| `static #Tensor<int64>[] nonzero<E extends Numeric>(Tensor<E> t)` | Per-dimension coordinate arrays of the nonzero elements (numpy `nonzero`) |
| `static #Tensor<E> extract<E extends Numeric>(Tensor<E> condition, Tensor<E> arr)` | Elements of the 1-D `arr` where `condition` is nonzero (numpy `extract`) |

### Elementwise arithmetic

All binary ops broadcast right-aligned. The auto-promote forms compute the
NEP-50 result dtype from the operand types; the explicit-`R` forms let the
caller pick the result width; the same-dtype forms keep `E`.

| Signature | |
|---|---|
| `static #Tensor<? extends Numeric> add<A extends Numeric, B extends Numeric>(Tensor<A> a, Tensor<B> b)` | Auto-promote `a + b` (NEP-50); bounded-wildcard result |
| `static #Tensor<? extends Numeric> sub<A extends Numeric, B extends Numeric>(Tensor<A> a, Tensor<B> b)` | Auto-promote `a - b` |
| `static #Tensor<? extends Numeric> mul<A extends Numeric, B extends Numeric>(Tensor<A> a, Tensor<B> b)` | Auto-promote `a * b` |
| `static #Tensor<? extends Floating> div<A extends Numeric, B extends Numeric>(Tensor<A> a, Tensor<B> b)` | Auto-promote true division `a / b`; floating result (`int/int → float64`) |
| `static #Tensor<E> add<E>(Tensor<E> a, Tensor<E> b)` | Elementwise `a + b`, fresh C-contiguous result |
| `static #Tensor<E> sub<E>(Tensor<E> a, Tensor<E> b)` | Elementwise `a - b` |
| `static #Tensor<E> mul<E>(Tensor<E> a, Tensor<E> b)` | Elementwise `a * b` |
| `static #Tensor<R> add<A extends Numeric, B extends Numeric, R extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype `a + b`; explicit result width `R` |
| `static #Tensor<R> sub<A extends Numeric, B extends Numeric, R extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype `a - b` |
| `static #Tensor<R> mul<A extends Numeric, B extends Numeric, R extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype `a * b` |
| `static #Tensor<R> div<A extends Numeric, B extends Numeric, R extends Floating>(Tensor<A> a, Tensor<B> b)` | True division; result `R` is floating (NEP-50 true division) |
| `static #Tensor<R> floorDiv<A extends Numeric, B extends Numeric, R extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype floor division `a // b` (toward −∞) |
| `static #Tensor<E> floorDiv<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Same-dtype floor division |
| `static #Tensor<R> mod<A extends Numeric, B extends Numeric, R extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype modulo `a % b` (sign of divisor) |
| `static #Tensor<E> mod<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Same-dtype modulo |

### Comparisons

| Signature | |
|---|---|
| `static #Tensor<boolean> eq<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Elementwise `a == b` |
| `static #Tensor<boolean> ne<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Elementwise `a != b` |
| `static #Tensor<boolean> lt<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Elementwise `a < b` |
| `static #Tensor<boolean> le<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Elementwise `a <= b` |
| `static #Tensor<boolean> gt<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Elementwise `a > b` |
| `static #Tensor<boolean> ge<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Elementwise `a >= b` |
| `static #Tensor<boolean> eq<A extends Numeric, B extends Numeric, C extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype `a == b` at explicit compare width `C` |
| `static #Tensor<boolean> ne<A extends Numeric, B extends Numeric, C extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype `a != b` |
| `static #Tensor<boolean> lt<A extends Numeric, B extends Numeric, C extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype `a < b` |
| `static #Tensor<boolean> le<A extends Numeric, B extends Numeric, C extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype `a <= b` |
| `static #Tensor<boolean> gt<A extends Numeric, B extends Numeric, C extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype `a > b` |
| `static #Tensor<boolean> ge<A extends Numeric, B extends Numeric, C extends Numeric>(Tensor<A> a, Tensor<B> b)` | Cross-dtype `a >= b` |

### Bitwise and logical

| Signature | |
|---|---|
| `static #Tensor<R> bitAnd<A extends Integral, B extends Integral, R extends Integral>(Tensor<A> a, Tensor<B> b)` | Cross-dtype bitwise AND; explicit integral width `R` |
| `static #Tensor<E> bitAnd<E extends Integral>(Tensor<E> a, Tensor<E> b)` | Same-dtype bitwise AND |
| `static #Tensor<R> bitOr<A extends Integral, B extends Integral, R extends Integral>(Tensor<A> a, Tensor<B> b)` | Cross-dtype bitwise OR |
| `static #Tensor<E> bitOr<E extends Integral>(Tensor<E> a, Tensor<E> b)` | Same-dtype bitwise OR |
| `static #Tensor<R> bitXor<A extends Integral, B extends Integral, R extends Integral>(Tensor<A> a, Tensor<B> b)` | Cross-dtype bitwise XOR |
| `static #Tensor<E> bitXor<E extends Integral>(Tensor<E> a, Tensor<E> b)` | Same-dtype bitwise XOR |
| `static #Tensor<R> shiftL<A extends Integral, B extends Integral, R extends Integral>(Tensor<A> a, Tensor<B> b)` | Cross-dtype left shift `a << b` |
| `static #Tensor<E> shiftL<E extends Integral>(Tensor<E> a, Tensor<E> b)` | Same-dtype left shift |
| `static #Tensor<R> shiftR<A extends Integral, B extends Integral, R extends Integral>(Tensor<A> a, Tensor<B> b)` | Cross-dtype right shift `a >> b` (arithmetic for signed) |
| `static #Tensor<E> shiftR<E extends Integral>(Tensor<E> a, Tensor<E> b)` | Same-dtype right shift |
| `static #Tensor<boolean> and(Tensor<boolean> a, Tensor<boolean> b)` | Elementwise logical AND of two boolean tensors |
| `static #Tensor<boolean> or(Tensor<boolean> a, Tensor<boolean> b)` | Elementwise logical OR |
| `static #Tensor<boolean> xor(Tensor<boolean> a, Tensor<boolean> b)` | Elementwise logical XOR |
| `static #Tensor<boolean> not(Tensor<boolean> a)` | Elementwise logical NOT |

### Elementwise math, select and scalar ops

| Signature | |
|---|---|
| `static #Tensor<E> sqrt<E extends Floating>(Tensor<E> a)` | Elementwise square root |
| `static #Tensor<E> sin<E extends Floating>(Tensor<E> a)` | Elementwise sine |
| `static #Tensor<E> cos<E extends Floating>(Tensor<E> a)` | Elementwise cosine |
| `static #Tensor<E> exp<E extends Floating>(Tensor<E> a)` | Elementwise exponential |
| `static #Tensor<E> log<E extends Floating>(Tensor<E> a)` | Elementwise natural log |
| `static #Tensor<E> floor<E extends Floating>(Tensor<E> a)` | Elementwise floor |
| `static #Tensor<E> ceil<E extends Floating>(Tensor<E> a)` | Elementwise ceil |
| `static #Tensor<E> round<E extends Floating>(Tensor<E> a)` | Elementwise round (round-half-up per `Math.round`) |
| `static #Tensor<E> neg<E extends Numeric>(Tensor<E> a)` | Elementwise negation |
| `static #Tensor<E> abs<E extends Numeric>(Tensor<E> a)` | Elementwise absolute value |
| `static #Tensor<R> where<A extends Numeric, B extends Numeric, R extends Numeric>(Tensor<boolean> cond, Tensor<A> a, Tensor<B> b)` | Elementwise select `cond ? a : b`, with three-way right-aligned broadcasting; result cross-cast to `R` |
| `static #Tensor<E> clip<E extends Numeric>(Tensor<E> t, E lo, E hi)` | Elementwise clamp of each element to `[lo, hi]`, keeping dtype `E` |
| `static #Tensor<E> addScalar<E extends Numeric>(Tensor<E> t, E s)` | Weak scalar `t + s` (keeps `E`) |
| `static #Tensor<E> subScalar<E extends Numeric>(Tensor<E> t, E s)` | Weak scalar `t - s` |
| `static #Tensor<E> mulScalar<E extends Numeric>(Tensor<E> t, E s)` | Weak scalar `t * s` |
| `static #Tensor<float64> addScalarF<E extends Integral>(Tensor<E> t, float64 s)` | Weak float scalar over an integer tensor `t + s` → `Tensor<float64>` |
| `static #Tensor<float64> subScalarF<E extends Integral>(Tensor<E> t, float64 s)` | Weak float scalar `t - s` → `Tensor<float64>` |
| `static #Tensor<float64> mulScalarF<E extends Integral>(Tensor<E> t, float64 s)` | Weak float scalar `t * s` → `Tensor<float64>` |

### Reductions and scans

| Signature | |
|---|---|
| `static R sum<E extends Numeric, R extends Numeric>(Tensor<E> t)` | Σ of all elements, accumulated in `R` |
| `static R prod<E extends Numeric, R extends Numeric>(Tensor<E> t)` | Π of all elements, accumulated in `R` |
| `static E min<E extends Numeric>(Tensor<E> t)` | Minimum over all elements |
| `static E max<E extends Numeric>(Tensor<E> t)` | Maximum over all elements |
| `static R mean<E extends Numeric, R extends Floating>(Tensor<E> t)` | Arithmetic mean over all elements in floating `R` |
| `static #Tensor<R> sumAxis<E extends Numeric, R extends Numeric>(Tensor<E> t, int32 axis, boolean keepdims)` | Σ along `axis` accumulated in `R` |
| `static #Tensor<R> prodAxis<E extends Numeric, R extends Numeric>(Tensor<E> t, int32 axis, boolean keepdims)` | Π along `axis` |
| `static #Tensor<E> minAxis<E extends Numeric>(Tensor<E> t, int32 axis, boolean keepdims)` | Minimum along `axis` (axis length >= 1) |
| `static #Tensor<E> maxAxis<E extends Numeric>(Tensor<E> t, int32 axis, boolean keepdims)` | Maximum along `axis` |
| `static #Tensor<R> meanAxis<E extends Numeric, R extends Floating>(Tensor<E> t, int32 axis, boolean keepdims)` | Mean along `axis` in floating `R` |
| `static int64 argmin<E extends Numeric>(Tensor<E> t)` | Flattened (C-order) index of the minimum; first occurrence on ties |
| `static int64 argmax<E extends Numeric>(Tensor<E> t)` | Flattened index of the maximum; first occurrence on ties |
| `static int64 countNonzero<E extends Numeric>(Tensor<E> t)` | Count of nonzero elements |
| `static boolean any<E extends Numeric>(Tensor<E> t)` | `true` iff any element is nonzero |
| `static boolean all<E extends Numeric>(Tensor<E> t)` | `true` iff every element is nonzero |
| `static boolean anyTrue(Tensor<boolean> t)` | `true` iff any element of a boolean mask is `true` |
| `static boolean allTrue(Tensor<boolean> t)` | `true` iff every element of a boolean mask is `true` |
| `static R variance<E extends Numeric, R extends Floating>(Tensor<E> t, int32 ddof)` | Variance in floating `R`: `Σ(x-μ)² / (n - ddof)` |
| `static R std<E extends Numeric, R extends Floating>(Tensor<E> t, int32 ddof)` | Standard deviation: `sqrt(var)` |
| `static R nansum<E extends Floating, R extends Floating>(Tensor<E> t)` | Σ over the non-NaN elements (numpy `nansum`) |
| `static R nanmean<E extends Floating, R extends Floating>(Tensor<E> t)` | Mean of the non-NaN elements (numpy `nanmean`) |
| `static #Tensor<R> cumsum<E extends Numeric, R extends Numeric>(Tensor<E> t)` | Cumulative sum over the C-order flattening (numpy `cumsum`) |
| `static #Tensor<R> cumprod<E extends Numeric, R extends Numeric>(Tensor<E> t)` | Cumulative product over the flattening (numpy `cumprod`) |
| `static #Tensor<R> cumsumAxis<E extends Numeric, R extends Numeric>(Tensor<E> t, int32 axis)` | Cumulative sum along `axis` |
| `static #Tensor<R> cumprodAxis<E extends Numeric, R extends Numeric>(Tensor<E> t, int32 axis)` | Cumulative product along `axis` |

### Indexing

| Signature | |
|---|---|
| `#Tensor<T> index(int32 axis, int64 i)` | Basic integer index along `axis` (negative wraps): a view with that axis removed |
| `#Tensor<T> sliceAxis(int32 axis, int64 start, int64 stop, int64 step)` | Basic slice `[start, stop)` along `axis` with a positive `step`: a view |
| `#Tensor<T> reverseAxis(int32 axis)` | Reverse `axis` (the `[::-1]` case): a view with negated stride |
| `#Tensor<T> maskedSelect(Tensor<boolean> mask)` | Boolean indexing (read): a 1-D copy of the elements where `mask` is true |
| `void maskedAssign(Tensor<boolean> mask, T value)` | Boolean indexing (write): set every element where `mask` is true to `value`, in place |
| `#Tensor<T> take(int64[] indices)` | Fancy indexing (gather): a 1-D copy holding `this[indices[k]]` along axis 0 |
| `void put(int64[] indices, T[] values)` | Fancy indexing (scatter): assign `this[indices[k]] = values[k]` along axis 0, in place |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/Tensor.cajeta`](../../../runtime/src/cajeta/math/Tensor.cajeta)
- [DType](DType.md) — the dtype descriptor and NEP-50 promotion
- [LinAlg](linalg/LinAlg.md) — factorizations, [Fft](fft/Fft.md) — Fourier transforms, [Stats](stats/Stats.md) — descriptive statistics
