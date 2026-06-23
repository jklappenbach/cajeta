# `cajeta.math.Tensor` — the keystone n-d array spec

> **Status: spec (requirements + the forever-API design decisions).** The `Tensor` is the
> permanent commitment everything numerical builds on (numpy/torch/keras all wrap an
> ndarray); its data model cannot churn later. This spec defines the **type + data model +
> storage/device model + interop seam + the op-dispatch seam** — *not* the operation
> library (elementwise/reductions/linalg/… are later phases in `numpy-porting-plan.md`,
> built against the seam this defines). Companion: `tensor-plan.md`.

## 1. Scope & role
`cajeta.math.Tensor` is the canonical, framework-neutral n-dimensional array — the lingua
franca every downstream numerical lib and the torch port shares (spec rationale:
`numpy-porting-spec.md` §1). It is **CPU-first** (fully usable with no GPU) and
GPU-accelerable (lowers to `cajeta.gpu`/`cajeta.gpu.xpu`). v1 delivers the *type and its
seams*; the op surface is layered on after.

## 2. The load-bearing design decisions

These three are permanent. Recommendations given; flagged for sign-off because they are
expensive to revisit.

### 2.1 Element dtype — **static generic `Tensor<T>`** (RESOLVED), runtime dtype via wildcards
numpy/torch make dtype a **runtime** property (one `ndarray` type; `x.dtype` is a field) —
largely a side-effect of Python being dynamically typed. cajeta is statically typed and
already has: element-generic types (`KernelBuffer<T>`, `Vector<T,N>`,
`cajeta.gpu.xpu.CooperativeMatrix<T,…>`); **reified, monomorphized generics**
(`docs/Embedded.md`; `cajeta.reflect.TemplateArgument`); and **first-class wildcards**
(`Stream<?>` in the stdlib; bounded `<? extends …>` — `docs/CaptureConversion.md`). So the
element type is a **compile-time parameter `Tensor<T>`**, and the runtime-dtype cases are
served by the language's own wildcards — **no separate erased `AnyTensor` type.**

- **`Tensor<T>`** — the concrete, static, monomorphized common case. Type-safe, no
  per-element dispatch, consistent with `KernelBuffer<T>` etc.
- **Mixed-dtype arithmetic is *not* a runtime-dtype problem.** `Tensor<int32> +
  Tensor<float64> → Tensor<float64>` resolves the result from the two *static* types via
  NEP-50 promotion (§2.3) + overloads. Static `Tensor<T>` covers it — this is the bulk of
  "mixed-type" and needs no airlock.
- **`Tensor<? extends Numeric>` (bounded wildcard)** — dtype-generic code (a routine over
  "any float tensor": `Tensor<? extends Floating>`) without runtime dispatch. Bounds:
  `Numeric` ⊃ `Floating` / `Integral` / `Complex` (§3).
- **`Tensor<?>` (unbounded wildcard) — the airlock** for dtype-known-only-at-runtime cases
  (`.npy` load, deserialize a checkpoint, the array protocol, dynamic-dtype FFI). Because
  generics are **reified**, a `Tensor<?>` carries its real `T`, so **capturing it back to
  `Tensor<T>`** (wildcard capture / a reflective `TemplateArgument` check) recovers the
  concrete dtype — no hand-rolled `DType`-tagged type needed. These boundary cases are a
  minority in compiled, statically-typed code; the static type is the norm, the wildcard is
  the boundary.

### 2.2 Rank & shape — **dynamic** (runtime ndim + shape) (RECOMMENDED)
Static rank (`Tensor<T, Rank>`) gives compile-time shape checks but is too rigid for real
numerical/ML code (`reshape`, runtime dims, variable batch). So **rank and shape are
runtime** (numpy's model): `ndim`, `shape[]`, `strides[]` are runtime fields. (A
static-rank companion for hot perf paths is a *possible later* addition, not v1.)
Recommend dynamic rank/shape.

### 2.3 Type promotion — **type-based (NEP-50)** (RECOMMENDED)
Use NumPy's modern **type-based** promotion (NEP-50: result dtype from operand *types*,
not their *values*), not the legacy value-based rules — cleaner, predictable, and a better
fit for a statically-typed language. Casting rides cajeta's existing `RoundingMode`-aware
cast brick.

## 3. dtype system
- **Native set (v1):** `bool`; `int8/16/32/64`; `uint8/16/32/64`; `float16`, `float32`,
  `float64`; `bfloat16`; `float8` variants + `fp4`/`fp6` (cajeta already has these native);
  `complex64`/`complex128`.
- **Deferred:** `float128`/`complex256` (extended precision), `datetime64`/`timedelta64`,
  structured/record dtypes. **Out of scope (never as a Tensor element):** `object` dtype.
- **dtype bounds (for wildcards):** marker bounds the native dtypes satisfy —
  `Numeric` ⊃ `Floating` (f16/f32/f64/bf16/fp4/fp6/fp8) · `Integral` (signed + unsigned
  ints) · `Complex` (complex64/128); `bool` stands alone. These are the bounds used by
  `Tensor<? extends Floating>` / `<? extends Numeric>` (§2.1) for dtype-generic code.
- **`DType` reflection value** — a runtime descriptor (kind, byte-size, signedness, the
  cajeta type) for promotion-table lookup and the wildcard-capture path. The static
  `Tensor<T>` carries its dtype in `T`; a `Tensor<?>` recovers its `DType` at runtime from
  its **reified type argument** (`TemplateArgument`) — no separate erased type.
- **Promotion table + cast** — the NEP-50 result-type table; `Tensor<T>.cast<U>()`
  (RoundingMode-aware).

## 4. Memory model: shape, strides, views, ownership
- **Layout:** a `Tensor<T>` is `{ Storage<T> data; int64 offset; int64[] shape; int64[]
  strides; }` — a *strided view* over a shared `Storage`. C-order default; F-order and
  arbitrary strides supported. `ndim`/`size`/`itemsize`/`nbytes` derived.
- **Views vs copies (the contract):** `reshape` (when stride-compatible), `transpose`,
  slicing, `broadcast_to`, `expand_dims`/`squeeze`, `.T`, basic indexing → **views**
  (share `Storage`, no copy). `copy()`, fancy/boolean indexing reads, `reshape` of a
  non-contiguous view → **copies**. The contract is explicit per-op and documented; a
  `Tensor` exposes `isView()`/`base()`/`isContiguous()`.
- **Ownership / RAII (cajeta storage-class axis):** `Storage<T>` is the owning,
  reference-counted backing (host buffer, + optional device `KernelBuffer<T>`); multiple
  `Tensor` views share one `Storage` via refcount; the buffer is released when the last
  view drops (the scope-exit drop chain). Per CajetaGPU.md's "one axis, one model": stack
  = copy / heap = ref; a `Tensor` value is an ordinary RAII handle over `Storage`.
- **Aliasing/overlap:** define overlap semantics for in-place ops on overlapping views
  (numpy's "may share memory" caution) — v1: in-place ops on self-overlapping views are
  defined-or-rejected, never UB.

## 5. Broadcasting & indexing
- **Broadcasting:** the standard right-aligned shape rule (dim equal, or one is 1, or
  absent) → a broadcast *view* (stride-0 on stretched axes), zero-copy. `broadcast_to`,
  `broadcast_arrays`; broadcast-shape computation is a shared primitive every binary op uses.
- **Indexing:**
  - *basic* — `int`, `slice` (start:stop:step, negative), `newaxis`, ellipsis → **view**.
  - *boolean* — mask (same-shape or broadcastable) → **copy** (read) / scatter (write);
    implemented via scan+compact (Phase ties to the op plan).
  - *fancy* — integer-array indexing (incl. multi-axis, `take_along_axis`) → **copy** /
    gather; write via scatter.
- The view-vs-copy result of each indexing form is part of the contract (§4).

## 6. Storage & device model (CPU-first, GPU-accelerable)
- **Default = host.** A `Tensor<T>` allocates host `Storage`; every operation has a CPU
  path; a build with no GPU is fully functional.
- **Device placement.** `Storage<T>` can additionally hold a `cajeta.gpu` device buffer
  (`KernelBuffer<T>`). `tensor.to(Device)`, `.cpu()`, `.gpu()` move/mirror; `.device()` reports
  placement. v1 policy: explicit placement (eager transfer); a lazy/auto-migrate policy is
  a later option, not v1.
- **The op-dispatch seam (defined here, exercised by the op phases).** A `Tensor` op
  resolves its execution path from operand placement + device availability: device-resident
  operands → a `cajeta.gpu` kernel lowering (elementwise/reduction/scan/`matmul`-on-
  `CooperativeMatrix`); host operands (or no GPU) → the portable CPU path. This is the SAME
  native-vs-portable, result-cross-checked discipline as `cajeta.gpu` (see numpy-porting-plan
  cross-cutting rule). The seam is the contract Phase-3+ ops plug into; v1 wires it and
  proves it with one elementwise op end-to-end, not the full op surface.

## 7. The interop protocol (the anti-fragmentation seam)
A `Tensor` **interop protocol/trait** (the array-protocol / DLPack analogue): exposes
`{ data ptr, dtype (DType), shape, strides, device, read-only flag }` so external libs and
the torch port share one array **zero-copy**, and so `cajeta.math.fft`/`linalg`/etc. and
downstream `cajeta.sci`/`cajeta.learn`/torch consume `Tensor` without conversion. Both
import (`Tensor.fromProtocol(...)`, zero-copy where layout permits) and export
(`tensor.protocol()`). This is the single biggest reason `Tensor` is canonical-in-stdlib —
it must exist in v1.

## 8. Public API surface (v1 — the type, not the op library)
Construction/factories sufficient to *exist and be tested*: `Tensor.of(...)`,
`zeros`/`ones`/`full`/`empty` (+`_like`), `arange`, `fromProtocol`, `fromBuffer` (wrap a
`KernelBuffer`/host buffer). Accessors: `shape`/`strides`/`ndim`/`size`/`dtype`/`device`/
`isContiguous`/`isView`/`base`. Structural (view-producing): `reshape`, `transpose`/`.T`,
`view`/slicing, `squeeze`/`expand_dims`, `broadcast_to`, `astype`/`cast`, `copy`,
`to`/`cpu`/`gpu`, indexing (basic/boolean/fancy), `protocol`/`fromProtocol`. **No
arithmetic/reductions/linalg here** — those are the op phases, layered on this surface.

## 9. Goals / Non-goals
**Goals:** the `Tensor<T>` type + the `Tensor<?>` / bounded-`<? extends Numeric>` wildcard
forms; the dtype system + NEP-50 promotion
+ cast; shape/strides/views with the explicit view-copy contract; broadcasting; indexing
(basic/boolean/fancy); CPU-first storage + device placement + the op-dispatch seam (proven
with one elementwise op); the interop protocol. RAII/refcount ownership.
**Non-goals (v1):** the operation library (elementwise/reduction/shape-op/contraction/sort
ufuncs — later phases); `linalg`/`fft`/`random`/`stats`; structured/object/datetime dtypes;
masked arrays; lazy/auto device migration; static-rank `Tensor<T,Rank>`; serialization
beyond the interop protocol.

## 10. Acceptance criteria
1. §2 decisions recorded (2.1 `Tensor<T>` primary + wildcard airlock — RESOLVED).
2. `Tensor<T>` exists with the §4 data model; views vs copies behave per the documented
   contract; a `Tensor<?>` captures back to `Tensor<T>` (reified) and `Tensor<? extends
   Floating>` admits any float tensor.
3. dtype set (§3) + NEP-50 promotion table + RoundingMode-aware cast, unit-tested vs the table.
4. Broadcasting + basic/boolean/fancy indexing match a numpy oracle on the canonical cases.
5. CPU-first: every §8 operation runs on a no-GPU build; device placement (`to`/`cpu`/`gpu`)
   works where a device exists; the op-dispatch seam is proven with one elementwise op
   cross-checked CPU vs GPU.
6. The interop protocol round-trips a `Tensor` zero-copy (export→import; and a buffer an
   external producer wrote is consumed without copy where layout permits).
7. The data-model + seam are complete enough that Phase-3 elementwise ops plug in with no
   `Tensor`-type changes (the forever-API is settled).
