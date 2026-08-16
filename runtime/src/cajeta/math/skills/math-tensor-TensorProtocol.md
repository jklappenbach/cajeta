---
id: math-tensor-TensorProtocol
applies-to: [cajeta/math/TensorProtocol]
title: TensorProtocol — zero-copy DLPack-analogue interop descriptor for Tensor
description: How to build, read, and consume a TensorProtocol to share a Tensor's backing zero-copy and memory-safe.
---

# TensorProtocol

A dtype-erased, **non-generic** descriptor of a tensor's backing — the cajeta
array-protocol / DLPack-capsule analogue. It carries
`{ Storage borrow, element offset, shape, strides, runtime DType, device, read-only }`
so a `Tensor` can be shared **zero-copy** with another cajeta `Tensor`, the torch
port, or an external library. The "data" field is a **borrow of the owning
`Storage`** (held erased as `Object`), not a raw pointer, so the round-trip is both
zero-copy and heap-safe.

**Support/value type, not an access point.** You do not normally `heap
TensorProtocol(...)` yourself. Get one from `Tensor.protocol()`; consume one with
`Tensor.fromProtocol(p)` / `Tensor.fromProtocolContiguous(p)`. Its public
constructor exists for external producers building a descriptor over a `Storage`
they already hold.

## Use it / don't

- Use `Tensor.protocol()` → read accessors → `Tensor.fromProtocol(...)` to
  round-trip or export a tensor without copying. See `cajeta/math/Tensor`.
- It does **not** own or copy the element buffer — it only describes one. The
  exporting `Tensor` keeps ownership; the buffer is freed once by the live-set.
- It is **not generic** — there is no `TensorProtocol<T>`. The element type is the
  runtime `dtype()` (see `cajeta/math/DType`).
- `fromProtocol` does **not** return your concrete type — it returns `Tensor<?>`.
  You must `instanceof`-guard and capture it back (reified-capture).
- `fromProtocol` does **not** support every dtype: fp8/fp6/fp4 and 128-bit
  variants are deferred and yield `null`.

## Worked example (mirrors TensorTests 7a — zero-copy round-trip)

```cajeta
import cajeta.math.Tensor;
import cajeta.math.TensorProtocol;
import cajeta.math.DType;

float32[] data = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
int64[] shp = heap int64[2];
shp[0] = 2;
shp[1] = 3;
Tensor<float32> t #= Tensor.of<float32>(data, shp);

TensorProtocol p #= t.protocol();              // export — borrows t's Storage
if (p.ndim() != 2) { /* ... */ }
DType pd = p.dtype();                          // runtime dtype, not a static T
if (!pd.isFloating() || pd.bits() != 32) { /* ... */ }
if (p.device() != 0) { /* ... */ }             // 0 = host (CPU)

Tensor<?> w #= Tensor.fromProtocol(p);          // import → Tensor<?> airlock
if (!(w instanceof Tensor<float32>)) { /* unsupported/wrong dtype */ }
Tensor<float32> back = (Tensor<float32>) w;    // reified-capture to concrete type

back.set2(0, 0, 9.0f);                          // write through the rebuilt view…
// t.get2(0, 0) == 9.0f                         // …shows through the original (shared, zero-copy)
```

## Construction & ownership

`public TensorProtocol(Object base, int64 offset, int64[] shape, int64[] strides,
DType dtype, int32 device, boolean readOnlyFlag)`

- `base` — the backing storage **erased as `Object`** (a borrow). It is **shared,
  not owned**: the producer keeps ownership. Upcasting `Storage<X>` → `Object` is
  the identity (same pointer); consumers downcast it back with `(Storage<E>)
  p.base()`.
- `shape` / `strides` — **copied** into freshly-owned arrays by the constructor, so
  the caller's local arrays drop cleanly afterward. Strides are in **elements**.
- `dtype` — pass with `#` transfer (a `#DType`); the exporter builds it via
  `DType.of<T>()`. See `cajeta/math/DType`.
- `device` — `0` host (CPU), `1` device (GPU). `readOnlyFlag` — producer's
  read-only mark.

## Accessors (all borrowed/by-value reads)

- `Object base()` — the borrowed backing, erased; downcast to `Storage<E>`.
- `int64 offset()`, `int32 ndim()`, `int64 shapeAt(int32)`, `int64 strideAt(int32)`.
- `#int64[] shapeCopy()` / `#int64[] strideCopy()` — fresh **owned** copies (the
  `#` return transfers ownership), for rebuilding a tensor.
- `DType dtype()` — borrowed runtime dtype; `int32 device()`; `boolean isReadOnly()`.

## The load-bearing design point (why `Object`, not `Storage<?>`)

The backing is held as `Object`, **deliberately not** as a `Storage<?>` wildcard.
An `Object` upcast is the identity, so teardown's virtual-drop reaches the *real*
`Storage` and is idempotent through the live-set. A `Storage<?>` wildcard field
would instead route through a materialized wildcard handle whose teardown
**corrupts the heap** when two distinct `Storage` instantiations are live at once
(the multi-`fromProtocol` regression). Do not change `baseStore` to `Storage<?>`.

Consumers (`rebuildShared` / `rebuildContiguous` in `cajeta/math/Tensor`) must pass
the borrow **inline** — `heap Tensor<E>((Storage<E>) p.base(), ...)` — never via a
named `Storage<E>` local, because a class-typed local bound to a borrow gets an
owning drop registered and would free the shared buffer at scope exit
(use-after-free).

## Lifecycle, errors, threading

- No `close()` / no disposal. As a plain object it drops on scope; dropping it does
  **not** free the described buffer (only its own copied shape/stride arrays).
- Holding a `TensorProtocol` past the lifetime of the exporting `Tensor`'s storage
  is unsafe — it is a borrow descriptor. Round-trip promptly.
- The constructor performs no validation and raises nothing. `Tensor.fromProtocol`
  returns `null` for an unsupported dtype — callers treat `null` as "cannot
  import". No fiber/thread-safety guarantees beyond the underlying `Storage`.
