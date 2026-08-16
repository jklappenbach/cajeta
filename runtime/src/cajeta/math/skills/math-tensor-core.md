---
id: math-tensor-core
applies-to: [cajeta/math/Tensor, cajeta/math/Storage, cajeta/math/TensorProtocol, cajeta/math/DType, cajeta/math/Cast, cajeta/math/RoundingMode, cajeta/math/BroadcastException]
title: Tensor core — strided Tensor over refcounted Storage, with dtype/cast/interop policies
description: How Tensor, Storage, TensorProtocol, DType and the policy enums cooperate — who owns the buffer, view-vs-copy, and the device/interop seams.
---

# Tensor core (the numpy ndarray unit)

This is cajeta's n-dimensional array as a cooperating set. **Start at `Tensor<T>`** —
everything else is reached through it. You almost never name `Storage` or
`TensorProtocol` directly; you construct via `Tensor.<factory>` and read the policy
enums where a method asks for one.

## Members and roles

- **`Tensor<T>`** — the entry point. A strided *view* `{ Storage<T> store; int64
  offset; int64[] shape; int64[] strides; }`. Element type is the **reified static
  `T`** (the dtype lives in `T`); rank/shape/strides are **runtime** fields, so
  `reshape`/views/broadcast are expressible. You only ever construct `Tensor`.
- **`Storage<T>`** — the owning host buffer (one contiguous `T[]`) that one *or more*
  `Tensor`s share. Internal: built by the `Tensor` factories, never by user code. Also
  holds the optional device mirror (`KernelBuffer<T>`, null until placed).
- **`DType`** — runtime dtype descriptor `(kind, bits, variant)`. Bridges the static
  `T` to a value via `DType.of<T>()`, and owns the **NEP-50 `promote(a,b)`** table that
  the (separately-shipped) op library reasons over.
- **`TensorProtocol`** — the dtype-erased interop seam (DLPack analogue). Carries a
  *borrow* of a `Storage` + offset/shape/strides/dtype/device/read-only for zero-copy
  hand-off. Built by `Tensor.protocol()`, consumed by `Tensor.fromProtocol(...)`.
- **`RoundingMode`** (cast policy enum),
  **`Cast`** (`roundToInt<I>(float64, RoundingMode)` — explicit rounding when the
  hardware default won't do), **`BroadcastException`** (recoverable; thrown by
  `broadcastShape`/`broadcastTo` on incompatible shapes).

## Object graph & who owns the buffer (the load-bearing rule)

```
 Tensor<T> ──store──▶ Storage<T> ──host──▶ T[]   (owning)
   │                       └──dev──▶ KernelBuffer<T> (device mirror, lazy)
   └─ alias()/reshape(view)/transpose/slice/... ─▶ Tensor<T> (sharing the SAME Storage)
```

A `heap Storage<T>` is auto-dropped: cajeta's global live-set frees the `T[]`
**exactly once**, even when several `Tensor`s share it. There is **no manual refcount
field** and no `close()`/`free()` to call — ownership is by the language's stack=copy /
heap=ref model. So:

- **Factories return an owned `#Tensor<T>`** that owns a fresh `Storage` (`#`-moved in).
- **Views borrow** the source's `Storage` (shared, not owned). `isView()` is true;
  `base()` is the owning root `Tensor` (or `null` for a shared view with no `Tensor`
  root — the interop rebuild case). Drop order does not matter; the buffer frees once.
- The factory-vs-view split is the whole safety story: a write through any view shows
  through every other view of that `Storage`.

## View vs copy (numpy's contract)

| op | result |
|---|---|
| `alias`, `transpose`, `slice`, `squeeze`, `expandDims`, `index`, `sliceAxis`, `reverseAxis`, `broadcastTo` | **view** (shares `Storage`) |
| `reshape` | **view if C-contiguous, else a copy** |
| `copy`, `maskedSelect`, `take` | **independent copy** (own `Storage`) |
| `maskedAssign`, `put`, `set*` | in-place write |

`broadcastTo` gives stretched axes **stride 0** (reads alias the same element) — reads
are well-defined, in-place writes through it are **not**.

## What this unit does NOT do

- **No op library here** — no `add`/`matmul`/reductions/`astype`. Those layer on this
  core (see `numpy-porting-plan`); the op routes itself using the seam below.
- **No 0-d scalar tensor** — indexing a 1-D tensor yields a 1-element 1-D view.
- **No negative-step slicing** in `slice`; use `reverseAxis` for `[::-1]`.
- **No `complex64/128`**, no fp8/fp6/fp4 (or 128-bit) `fromProtocol` rebuild yet —
  `fromProtocol` returns `null` for an unsupported dtype.
- **Device transfer is eager and explicit** (`gpu()`/`cpu()`), not automatic.

## Cross-class workflows

### Construct + access (the everyday path)
```cajeta
import cajeta.math.Tensor;

int64[] shp = heap int64[2];
shp[0] = 2;
shp[1] = 3;
Tensor<float32> z #= Tensor.zeros<float32>(shp);      // owned #Tensor, C-order, strides [3,1]
Tensor<int32>   r #= Tensor.arange<int32>(5);         // 1-D [0,1,2,3,4]
z.set2(1, 2, 9.0f);                                  // in-place write
Tensor<float32> v #= z.transpose();                   // view: v.isView()==true, shares z's Storage
```
Factories are **method-templated statics** — pass the element type explicitly:
`Tensor.zeros<float32>(shp)`, `Tensor.full<int32>(shp, 7)`, `Tensor.of<int32>(data, shp)`,
plus the `_like` forms (`Tensor.zerosLike<int32>(src)`). Shape arrays are `heap
int64[]` built by hand. `of<E>` copies the input data into a fresh `Storage`.

### dtype & promotion
```cajeta
import cajeta.math.DType;

DType dt #= DType.of<float32>();          // == DType.f32(); reified from T
DType rt #= DType.promote(DType.i32(), DType.f32());  // NEP-50: float32 (int absorbed by float, width kept)
```
`DType.of<T>()` is a codegen intrinsic folded from `T`. Named factories use short names
(`i32`/`f32`/`u8`/`bf16`/`f8e4m3`…) because the real primitive names are reserved
keywords. All `DType` factories return an owned `#DType`.

### Interop round-trip (zero-copy) — the Tensor<?> airlock
```cajeta
import cajeta.math.Tensor;
import cajeta.math.TensorProtocol;

TensorProtocol p #= t.protocol();         // export: you own the descriptor; it BORROWS t's Storage + metadata
Tensor<?> w #= Tensor.fromProtocol(p);     // rebuild a Tensor<X> sharing the SAME Storage, widened to Tensor<?>
if (w instanceof Tensor<float32>) {       // reified dtype guard
    Tensor<float32> back = (Tensor<float32>) w;   // reified-capture to the concrete type
    back.set2(0, 0, 9.0f);                // write shows through t (zero-copy)
}
```
Ownership across the interop boundary: `TensorProtocol` holds the backing **erased as
`Object`** (a borrow — the exporting `Tensor` keeps ownership; the live-set still frees
once). `fromProtocol` returns a `#Tensor<?>` that you **capture** back to the concrete
`Tensor<X>` via an `instanceof`-guarded cast; it is a **shared view** (`base()==null`).
Returns `null` for a dtype outside the rebuilt set. Use `fromProtocolContiguous` for an
external producer: zero-copy view when already C-contiguous, an independent **copy**
otherwise.

### Device seam (the op-dispatch routing point)
```cajeta
a.gpu();                  // eager mirror host->device; a.isOnGpu() now true, a.device()==1
KernelBuffer<float32> buf = a.deviceBuffer();   // null until gpu(); bind into a @Kernel launch
a.cpu();                  // bring back to host
```
An op picks its path by reading `isOnGpu()`/`deviceBuffer()` (GPU) vs `flatGet`/`flatSet`
(contiguous CPU loop). A no-GPU build still works — `cajeta.xpu`'s CPU backend backs the
buffer.

## Errors
`broadcastShape<E>(a,b)` and `broadcastTo(target)` throw **`BroadcastException`** (a
`RecoverableException` — catch it, it's a normal caller-handleable shape mismatch) when
right-aligned axes are incompatible, or `broadcastTo` is given fewer dims than the source.
Element accessors do no bounds checking beyond the underlying array.

## See also
Per-class detail (full method tables, every dtype factory, NEP-50 rule list) lives in
the individual class skills for `cajeta/math/Tensor`, `cajeta/math/DType`, and
`cajeta/math/Storage`; this skill only wires them together.
