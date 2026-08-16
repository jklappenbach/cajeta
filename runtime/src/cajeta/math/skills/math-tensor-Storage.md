---
id: math-tensor-Storage
applies-to: [cajeta/math/Storage]
title: Storage<T> — a Tensor's owning host buffer + device mirror
description: Internal owning host backing of a Tensor; auto-dropped, freed once across shared views, plus the toDevice/toHost device-mirror seam ops read.
---

# `Storage<T>` — owning host backing of a `Tensor<T>`

**Support / internal type — you do not construct it.** It is the single contiguous
`T[]` heap buffer that one *or more* `Tensor<T>` views share. Reach it only through
`Tensor` factories (`Tensor.zeros<E>`, `arange<E>`, `empty<E>`, …) and `Tensor`
methods; ordinary tensor code never names `Storage`. The two reasons to read this
page: you are inside `cajeta.math` writing a factory/op, or you need the exact
**lifecycle** (who frees the buffer) and the **device-residency seam** an op uses to
pick CPU vs GPU.

## What it does / does NOT do
- **Owns** one host `T[]` of `length` elements and an *optional* device mirror.
- It does **not** know about shape, strides, offset, or dtype — those live on
  `Tensor` (see `cajeta/math/Tensor`). `Storage` only does flat `get`/`set` by
  linear buffer index.
- It does **not** refcount. There is no `addRef`/`release` and no manual count
  field; sharing is handled by the runtime live-set (see Lifecycle).
- `get`/`set` do **no bounds checking** and read/write the **host** buffer only —
  they do not consult `onDevice`. Stale reads are possible if data is device-resident
  and dirty; call `toHost()` first.

## Construction & ownership (internal)
```
package cajeta.math;

// inside a Tensor factory:
int64 n = Tensor.productOf(shape);
Storage<E> st = heap Storage<E>(n);   // zero-initialized host buffer, host-resident
return heap Tensor<E>(#st, 0, shape, strides);  // #st: ownership transfers INTO the Tensor
```
`Storage(int64 length)` allocates a zero-filled `heap T[length]`, with `dev = null`
and `onDevice = false`. The `#st` at the `Tensor(...)` call site **moves** the owned
`Storage` handle into the tensor; do not keep using `st` after the transfer.

## Methods that matter
- `int64 size()` — element count (`length`).
- `T get(int64 i)` / `void set(int64 i, T v)` — flat **host** access by linear buffer
  index (callers add the tensor's offset). Returns a value copy; no null.
- `void toDevice()` — mirror host → device, allocating the `KernelBuffer<T>` on first
  call, then set `onDevice = true`. Idempotent (no-op if already on device). Eager
  (v1 policy).
- `void toHost()` — copy device → host, set `onDevice = false`. No-op if already host.
- `boolean isOnDevice()` — true when live data is on the device.
- `KernelBuffer<T> deviceBuffer()` — the device mirror, **borrowed, nullable**: `null`
  until the first `toDevice()`. Owned by this `Storage`; do not free it. See
  `cajeta/gpu/KernelBuffer`.

## The device-dispatch seam
An op chooses its path by reading the seam (via the owning `Tensor`, which delegates:
`Tensor.gpu()→toDevice`, `cpu()→toHost`, `isOnGpu()→isOnDevice`, `deviceBuffer()`):
```
import cajeta.math.Tensor;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;

public static #Tensor<float32> add(Tensor<float32> a, Tensor<float32> b) {
    if (a.isOnGpu() && b.isOnGpu()) {        // both device-resident → GPU kernel
        Tensor<float32> out #= Tensor.zeros<float32>(a.shape());
        out.gpu();
        KernelStream s #= KernelStream.current();
        // dispatch a @Kernel over a.deviceBuffer(), b.deviceBuffer(), out.deviceBuffer()
        return out;
    }
    // ... CPU path over host get/set ...
}
```
A no-GPU build still works: the `cajeta.xpu` CPU backend backs the buffer, so
`toDevice`/`deviceBuffer` remain valid.

## Lifecycle — freed exactly once, no manual refcount
`Storage` follows cajeta's "one axis, one model": a `heap Storage<T>` is
**auto-dropped** at scope exit, and the global live-set claim frees the underlying
buffer **exactly once** even when several `Tensor` views (e.g. from `alias()` /
`TensorProtocol` rebuilds) share it. Consequences for `cajeta.math` authors:
- Never `free()` the host buffer by hand and never write a refcount.
- When passing a **shared/borrowed** `Storage` into a new `Tensor` (e.g.
  `fromProtocol` zero-copy rebuild), pass it **directly** — do not bind it to a named
  `Storage<E>` local first, or that local's virtual-drop at scope exit will free the
  shared buffer and cause a use-after-free in the other views.
- The device mirror (`dev`) is owned by the `Storage` and torn down with it; ops must
  not free `deviceBuffer()`.

## State & concurrency
Mutable (host contents, `dev`, `onDevice` all change). Shared across tensor views by
design, so it is **not** thread/fiber-safe — external synchronization is the caller's
job. `toDevice`/`toHost` are idempotent but mutate residency state.

## Errors
The methods raise no `cajeta.math` exceptions themselves. Out-of-range `get`/`set`
indices are undefined (no bounds check). Device transfer surfaces failures from
`cajeta.xpu.KernelBuffer` (`upload`/`download`).

## See also
- `cajeta/math/Tensor` — the view that owns and exposes this `Storage`.
- `cajeta/gpu/KernelBuffer` — the device mirror type and its upload/download contract.
