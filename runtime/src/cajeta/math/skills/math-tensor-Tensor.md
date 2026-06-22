---
id: math-tensor-Tensor
applies-to: [cajeta/math/Tensor]
title: Tensor — n-d strided view over a shared, refcounted Storage
description: The main tensor access point — method-templated factories (zeros/ones/full/empty/arange/of + _like), the strided-view fields, RAII ownership, and the alias/shared-Storage drop-chain.
---

# Tensor&lt;T&gt;

The keystone n-dimensional array of `cajeta.math`: a **strided view** over a shared,
refcounted `Storage<T>` — the four view fields are `store` (the backing), `offsetVal`
(element offset into it), `shapeDims`, and `strideDims` (both element-unit `int64[]`).
`T` is the static, reified element type (the dtype lives in `T`; see `cajeta.math.DType`);
rank and shape are **runtime** data, so reshape and variable dims are expressible (numpy's
model).

**Access point: yes — this is "start here" for `cajeta.math`.** You do not call the
constructor directly (it is public but internal — it `#`-moves in a `Storage` the
factories build). Obtain every tensor through a factory.

## Construction — method-templated statics, explicit element type

The factories are **method-templated**: pass the element type at the call, not on a
variable. C-order (row-major) is the layout default.

```cajeta
import cajeta.math.Tensor;
import cajeta.math.MemoryOrder;

int64[] shp = heap int64[2];
shp[0] = 2;
shp[1] = 3;
Tensor<float32> z = Tensor.zeros<float32>(shp);   // 2x3, strides [3,1]
Tensor<int32>   o = Tensor.ones<int32>(shp);
Tensor<int32>   f = Tensor.full<int32>(shp, 7);   // value arg typed E
Tensor<int32>   r = Tensor.arange<int32>(6);      // 1-D [0,1,2,3,4,5]
int32[] data = { 10, 20, 30, 40, 50, 60 };
Tensor<int32>   t = Tensor.of<int32>(data, shp);  // copies data into fresh Storage
Tensor<float32> ff = Tensor.emptyOrdered<float32>(shp, MemoryOrder.F); // F-order, strides [1,2]
```

The factory set: `zeros<E>(shape)`, `ones<E>(shape)`, `full<E>(shape, value)`,
`empty<E>(shape)` (host buffer is zero-filled despite the name),
`emptyOrdered<E>(shape, order)` (the only C/F-order knob), `arange<E>(n)` (1-D),
`of<E>(data, shape)` (copies the leading `productOf(shape)` elements of `data`). The
`_like` forms take a source tensor and **also require the explicit element type**:
`zerosLike<E>(src)`, `onesLike<E>(src)`, `fullLike<E>(src, value)`.

Each factory **allocates its own `Storage` and returns an owned `#Tensor<E>`** — the
return crosses to you; the tensor owns its backing.

## Ownership & lifecycle (RAII) — the drop-chain

A `Tensor` is an ordinary handle. A factory-built tensor owns its `Storage`; an
**alias** is a non-owning view that *shares* one tensor's `Storage` by the global
live-set claim. There is **no `close()`** and no manual free — handles drop on scope
exit, and the shared buffer is freed **exactly once** when the last sharer drops (no
refcount field; per `Storage`'s "heap = ref, one live-set claim" rule). So you may let
any number of aliases all fall out of scope together — no leak, no double-free.

```cajeta
import cajeta.math.Tensor;

Tensor<int32> a = Tensor.arange<int32>(4);   // [0,1,2,3]; a owns the Storage
Tensor<int32> b = a.alias();                 // #Tensor — non-owning whole-array view
// b.isView() == true,  a.isView() == false
// b.base()   == a,     a.base()   == null
a.set1(2, 99);                               // write through a ...
int32 x = b.get1(2);                         // ... shows through b: 99
Tensor<int32> c = a.alias();
Tensor<int32> d = b.alias();                 // base() of an alias-of-alias is the owning root
// a, b, c, d all drop here — buffer freed once
```

`alias()` returns an owned `#Tensor<T>` whose `store` is borrowed from the source;
`base()` returns the owning root tensor (the source, or the source's base if it is itself
a view) or **null** when the tensor owns its storage. `isView()` distinguishes the two.
A few accessors also hand back fresh owned arrays — `shape()` returns `#int64[]` (a copy;
mutating it does not touch the tensor), and `dtype()` returns a `#DType`.

## Accessors that matter

`ndim()`, `size()` (element count), `shapeAt(axis)`, `strideAt(axis)`, `offset()`,
`itemsize()`/`nbytes()`, `isContiguous()` (C-contiguity test). Element access:
`getAt(idx)`/`setAt(idx, v)` (multi-index, `idx.count()` must equal `ndim`),
`get1`/`set1`, `get2`/`set2`, and `flatGet`/`flatSet` (linear by storage index — valid
on a contiguous tensor). These return/store `T` by its normal value/reference convention.

## State, concurrency, sharp edges

- **Mutable and not thread/fiber-safe.** Writes through any sharer are visible through
  every sharer of the same `Storage` — that is the point of `alias()`, but it means
  concurrent mutation needs external synchronization.
- **No bounds checks** on shapes/indices in v1 — an out-of-range multi-index or axis
  reads/writes raw backing storage.
- `idx` length for `getAt`/`setAt` must equal `ndim()`; `flatGet`/`flatSet` assume
  contiguity.

## What this skill does NOT cover

The view/copy structural ops (`reshape`, `transpose`, `slice`, `squeeze`, `expandDims`,
`copy`), broadcasting (`broadcastShape`/`broadcastTo`, which throw
`cajeta.math.BroadcastException`), the indexing surface (`index`/`sliceAxis`/
`reverseAxis`/`maskedSelect`/`maskedAssign`/`take`/`put`), device placement
(`gpu`/`cpu`/`deviceBuffer`, see `cajeta.math.Storage`), and the zero-copy interop seam
(`protocol`/`fromProtocol`, see `cajeta.math.TensorProtocol`) are all layered on these
fields and ownership rules but documented elsewhere. `alias()` is the only view exercised
here, to demonstrate the shared-Storage drop-chain.
