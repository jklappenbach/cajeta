# `cajeta.math.Tensor` — the keystone n-d array spec

> **Status: spec (requirements + the forever-API design decisions).** The `Tensor` is the
> permanent commitment everything numerical builds on, since numpy, torch and keras all
> wrap an ndarray, and its data model cannot churn later. This spec defines the type, the
> data model, the storage/device model, the interop seam and the op-dispatch seam. It does
> not define the operation library. Elementwise, reductions, linalg and the rest are later
> phases in `numpy-porting-plan.md`, built against the seam this defines. The companion is
> `tensor-plan.md`.

## 1. Scope & role
`cajeta.math.Tensor` is the canonical, framework-neutral n-dimensional array. It is the
lingua franca every downstream numerical lib and the torch port shares (the spec rationale
is in `numpy-porting-spec.md` §1). It is CPU-first and fully usable with no GPU, and it is
GPU-accelerable, lowering to `cajeta.xpu`/`cajeta.xpu.xpu`. v1 delivers the type and its
seams, and the op surface is layered on after.

## 2. The load-bearing design decisions

These three are permanent. Recommendations are given, and they are flagged for sign-off
because they are expensive to revisit.

### 2.1 Element dtype — **static generic `Tensor<T>`** (RESOLVED), runtime dtype via wildcards
numpy and torch make dtype a runtime property, with one `ndarray` type and `x.dtype` as a
field. That is largely a side-effect of Python being dynamically typed. cajeta is statically
typed and already has element-generic types (`KernelBuffer<T>`, `Vector<T,N>`,
`cajeta.xpu.xpu.CooperativeMatrix<T,…>`), reified and monomorphized generics
(`docs/Embedded.md`; `cajeta.reflect.TemplateArgument`), and first-class wildcards
(`Stream<?>` in the stdlib; bounded `<? extends …>`, see `docs/CaptureConversion.md`). So the
element type is a compile-time parameter `Tensor<T>`, and the runtime-dtype cases are
served by the language's own wildcards. There is no separate erased `AnyTensor` type.

- **`Tensor<T>`** is the concrete, static, monomorphized common case. It is type-safe, has
  no per-element dispatch, and is consistent with `KernelBuffer<T>` and the others.
- **Mixed-dtype arithmetic is *not* a runtime-dtype problem.** `Tensor<int32> +
  Tensor<float64> → Tensor<float64>` resolves the result from the two static types via
  NEP-50 promotion (§2.3) and overloads. Static `Tensor<T>` covers it. This is the bulk of
  "mixed-type" and needs no airlock.
- **`Tensor<? extends Numeric>` (bounded wildcard)** serves dtype-generic code, such as a
  routine over "any float tensor" (`Tensor<? extends Floating>`), without runtime dispatch.
  The bounds are `Numeric` ⊃ `Floating` / `Integral` / `Complex` (§3).
- **`Tensor<?>` (unbounded wildcard) — the airlock** for the cases where the dtype is known
  only at runtime (`.npy` load, deserialize a checkpoint, the array protocol, dynamic-dtype
  FFI). Because generics are reified, a `Tensor<?>` carries its real `T`, so capturing it
  back to `Tensor<T>` (wildcard capture / a reflective `TemplateArgument` check) recovers
  the concrete dtype, and no hand-rolled `DType`-tagged type is needed. These boundary cases
  are a minority in compiled, statically-typed code. The static type is the norm, and the
  wildcard is the boundary.

### 2.2 Rank & shape — **dynamic** (runtime ndim + shape) (RECOMMENDED)
Static rank (`Tensor<T, Rank>`) gives compile-time shape checks but is too rigid for real
numerical and ML code (`reshape`, runtime dims, variable batch). So rank and shape are
runtime, which is numpy's model. `ndim`, `shape[]` and `strides[]` are runtime fields. A
static-rank companion for hot perf paths is a possible later addition and is not v1. The
recommendation is dynamic rank/shape.

### 2.3 Type promotion — **type-based (NEP-50)** (RECOMMENDED)
Use NumPy's modern type-based promotion (NEP-50), where the result dtype comes from the
operand types rather than their values, instead of the legacy value-based rules. It is
cleaner, predictable, and a better fit for a statically-typed language. Casting rides
cajeta's existing `RoundingMode`-aware cast brick.

## 3. dtype system
- **Native set (v1):** `bool`; `int8/16/32/64`; `uint8/16/32/64`; `float16`, `float32`,
  `float64`; `bfloat16`; `float8` variants + `fp4`/`fp6` (cajeta already has these native);
  `complex64`/`complex128`.
- **Deferred:** `float128`/`complex256` (extended precision), `datetime64`/`timedelta64`,
  structured/record dtypes. **Out of scope (never as a Tensor element):** `object` dtype.
- **dtype bounds (for wildcards):** marker bounds the native dtypes satisfy. `Numeric` ⊃
  `Floating` (f16/f32/f64/bf16/fp4/fp6/fp8) · `Integral` (signed + unsigned ints) ·
  `Complex` (complex64/128), and `bool` stands alone. These are the bounds used by
  `Tensor<? extends Floating>` / `<? extends Numeric>` (§2.1) for dtype-generic code.
- **`DType` reflection value.** A runtime descriptor (kind, byte-size, signedness, the
  cajeta type) for promotion-table lookup and the wildcard-capture path. The static
  `Tensor<T>` carries its dtype in `T`. A `Tensor<?>` recovers its `DType` at runtime from
  its reified type argument (`TemplateArgument`), so there is no separate erased type.
- **Promotion table + cast.** The NEP-50 result-type table, and `Tensor<T>.cast<U>()`
  (RoundingMode-aware).

## 4. Memory model: shape, strides, views, ownership
- **Layout.** A `Tensor<T>` is `{ Storage<T> data; int64 offset; int64[] shape; int64[]
  strides; }`, a strided view over a shared `Storage`. C-order is the default, and F-order
  and arbitrary strides are supported. `ndim`/`size`/`itemsize`/`nbytes` are derived.
- **Views vs copies (the contract).** `reshape` (when stride-compatible), `transpose`,
  slicing, `broadcast_to`, `expand_dims`/`squeeze`, `.T` and basic indexing produce views,
  which share `Storage` with no copy. `copy()`, fancy/boolean indexing reads, and `reshape`
  of a non-contiguous view produce copies. The contract is explicit per-op and documented,
  and a `Tensor` exposes `isView()`/`base()`/`isContiguous()`.
- **Ownership / RAII (cajeta storage-class axis).** `Storage<T>` is the owning,
  reference-counted backing (host buffer, plus an optional device `KernelBuffer<T>`).
  Multiple `Tensor` views share one `Storage` via refcount, and the buffer is released when
  the last view drops through the scope-exit drop chain. Per CajetaGPU.md's "one axis, one
  model", stack = copy and heap = ref, and a `Tensor` value is an ordinary RAII handle over
  `Storage`.
- **Aliasing/overlap.** Define overlap semantics for in-place ops on overlapping views
  (numpy's "may share memory" caution). In v1, in-place ops on self-overlapping views are
  defined-or-rejected and never UB.

## 5. Broadcasting & indexing
- **Broadcasting.** The standard right-aligned shape rule (dim equal, or one is 1, or
  absent) yields a broadcast view with stride-0 on stretched axes, zero-copy. `broadcast_to`
  and `broadcast_arrays` are provided, and broadcast-shape computation is a shared primitive
  every binary op uses.
- **Indexing:**
  - *basic* — `int`, `slice` (start:stop:step, negative), `newaxis`, ellipsis → **view**.
  - *boolean* — mask (same-shape or broadcastable) → **copy** (read) / scatter (write);
    implemented via scan+compact (Phase ties to the op plan).
  - *fancy* — integer-array indexing (incl. multi-axis, `take_along_axis`) → **copy** /
    gather; write via scatter.
- The view-vs-copy result of each indexing form is part of the contract (§4).

## 6. Storage & device model (CPU-first, GPU-accelerable)
- **Default = host.** A `Tensor<T>` allocates host `Storage`. Every operation has a CPU
  path, and a build with no GPU is fully functional.
- **Device placement.** `Storage<T>` can additionally hold a `cajeta.xpu` device buffer
  (`KernelBuffer<T>`). `tensor.to(Device)`, `.cpu()` and `.gpu()` move or mirror, and
  `.device()` reports placement. The v1 policy is explicit placement with eager transfer.
  A lazy/auto-migrate policy is a later option and is not v1.
- **The op-dispatch seam (defined here, exercised by the op phases).** A `Tensor` op
  resolves its execution path from operand placement and device availability.
  Device-resident operands take a `cajeta.xpu` kernel lowering (elementwise/reduction/scan/
  `matmul`-on-`CooperativeMatrix`). Host operands, or no GPU, take the portable CPU path.
  This is the SAME native-vs-portable, result-cross-checked discipline as `cajeta.xpu` (see
  the numpy-porting-plan cross-cutting rule). The seam is the contract Phase-3+ ops plug
  into. v1 wires it and proves it with one elementwise op end-to-end, and it does not
  deliver the full op surface.

## 7. The interop protocol (the anti-fragmentation seam)
A `Tensor` interop protocol/trait, the array-protocol / DLPack analogue, exposes
`{ data ptr, dtype (DType), shape, strides, device, read-only flag }`. So external libs and
the torch port share one array zero-copy, and `cajeta.math.fft`/`linalg`/etc. and
downstream `cajeta.sci`/`cajeta.learn`/torch consume `Tensor` without conversion. Both
import (`Tensor.fromProtocol(...)`, zero-copy where layout permits) and export
(`tensor.protocol()`) are provided. This is the single biggest reason `Tensor` is
canonical-in-stdlib, and it must exist in v1.

## 8. Public API surface (v1 — the type, not the op library)
The construction/factories are enough for the type to exist and be tested: `Tensor.of(...)`,
`zeros`/`ones`/`full`/`empty` (+`_like`), `arange`, `fromProtocol`, `fromBuffer` (wrap a
`KernelBuffer`/host buffer). The accessors are `shape`/`strides`/`ndim`/`size`/`dtype`/
`device`/`isContiguous`/`isView`/`base`. The structural (view-producing) surface is
`reshape`, `transpose`/`.T`, `view`/slicing, `squeeze`/`expand_dims`, `broadcast_to`,
`astype`/`cast`, `copy`, `to`/`cpu`/`gpu`, indexing (basic/boolean/fancy), and
`protocol`/`fromProtocol`. There is no arithmetic, no reductions and no linalg here. Those
are the op phases, layered on this surface.

## 9. Goals / Non-goals
**Goals:** the `Tensor<T>` type + the `Tensor<?>` / bounded-`<? extends Numeric>` wildcard
forms; the dtype system + NEP-50 promotion
+ cast; shape/strides/views with the explicit view-copy contract; broadcasting; indexing
(basic/boolean/fancy); CPU-first storage + device placement + the op-dispatch seam (proven
with one elementwise op); the interop protocol. RAII/refcount ownership.
**Non-goals (v1):** the operation library (elementwise/reduction/shape-op/contraction/sort
ufuncs, which are later phases); `linalg`/`fft`/`random`/`stats`; structured/object/datetime dtypes;
masked arrays; lazy/auto device migration; static-rank `Tensor<T,Rank>`; serialization
beyond the interop protocol.

## 10. Acceptance criteria
1. §2 decisions recorded (2.1 `Tensor<T>` primary + wildcard airlock, RESOLVED).
2. `Tensor<T>` exists with the §4 data model. Views vs copies behave per the documented
   contract, a `Tensor<?>` captures back to `Tensor<T>` (reified), and `Tensor<? extends
   Floating>` admits any float tensor.
3. dtype set (§3) + NEP-50 promotion table + RoundingMode-aware cast, unit-tested vs the table.
4. Broadcasting + basic/boolean/fancy indexing match a numpy oracle on the canonical cases.
5. CPU-first: every §8 operation runs on a no-GPU build. Device placement (`to`/`cpu`/`gpu`)
   works where a device exists, and the op-dispatch seam is proven with one elementwise op
   cross-checked CPU vs GPU.
6. The interop protocol round-trips a `Tensor` zero-copy (export→import; and a buffer an
   external producer wrote is consumed without copy where layout permits).
7. The data-model + seam are complete enough that Phase-3 elementwise ops plug in with no
   `Tensor`-type changes (the forever-API is settled).
