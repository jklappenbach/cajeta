# Kernel-aware vectorized load/store

> Status: DRAFT (pending developer approval). Specifies width-generic packed SIMD
> load/store against a `KernelBuffer<T>` from inside an `@Kernel`, across every element
> type and lane width `Vector<T,N>` supports, on every XPU backend.

## 1. Definition

### 1.1 Purpose
Inside an `@Kernel`, there is no way to load N contiguous buffer elements into a
`Vector<T,N>` (or store one back) as a single packed SIMD operation. `Vector<T,N>`
arithmetic already lowers to packed LLVM vectors, and the host-side `Cajeta.vload8f64`
intrinsic does packed loads from a host **array** — but that intrinsic assumes the host
array header layout (`{i64 size, [0 x double]}`, data at byte offset 8), which a
`KernelBuffer` does **not** have (on the CPU backend it is a raw device pointer; on GPU
backends a descriptor binding). So a kernel can only build a vector element-by-element
from scalar loads, which the backend does not SLP-fold — the kernel runs **scalar**.

### 1.2 Problem it solves (motivating evidence)
The CPU `@Kernel` matmul (`samples/matmul-kernel/`) builds `Vector<float64,8>` from eight
scalar `b[bb+i]` loads. Measured: the *serial* kernel is **2.776 ms** (n=200) — 6x slower
than the host single-core SIMD matmul (**0.448 ms**, which uses `Cajeta.vload8f64`) —
because the kernel never vectorizes. 32-core parallelism then only claws scalar back to
**0.434 ms**, ~= the single-core SIMD version: the SIMD and the parallelism cancel instead
of compounding. With a real packed load inside the kernel the two multiply
(SIMD ~6x x parallel ~6.4x) to an estimated ~0.07 ms — beating numpy's 0.079 ms.

### 1.3 Scope
A width-generic vectorized **load** and **store** on `KernelBuffer<T>`, callable in
`@Kernel` bodies, covering: every element type `T` for which `Vector<T,N>` is valid; every
lane width `N` `Vector<T,N>` permits; and all four XPU backends (CPU, Vulkan/SPIR-V, AMD,
NVPTX). Includes the enabling compiler feature — **method-level const-generic parameters** —
that the `vload<N>` surface requires.

### 1.4 Constraints
- 1.4.1 Reuse the existing `Vector<T,N>` packed lowering (`<N x T>`) and the existing
  per-backend buffer-addressing seam (`bufferElementPtr`) — do not fork buffer addressing.
- 1.4.2 Backend-portable by construction: where a backend cannot natively load width N
  (e.g. SPIR-V `OpTypeVector` capped at 4 components, NVPTX `ld.global.v{2,4}`), lower to
  the minimum number of native-width ops and recombine into the one `Vector<T,N>` value —
  identical observable semantics across backends.
- 1.4.3 Contiguous access only (a base element index + N consecutive elements).
- 1.4.4 A kernel-body construct: the operations address device memory and are only
  meaningful inside an `@Kernel`.

### 1.5 Non-goals
- 1.5.1 Auto-vectorization of scalar kernel code — this is *explicit* SIMD the developer writes.
- 1.5.2 Strided / gather / scatter loads — contiguous only (a separate future feature).
- 1.5.3 Cross-lane shuffles or reductions beyond what `Vector<T,N>` already provides.
- 1.5.4 Reworking the host-array `Cajeta.vload8f64` intrinsic is **out of required scope**;
  §7 captures host-array parity as an optional, clearly-separable use case.

---

## 2. Method-level const-generic parameters (enabling machinery)

`Vector<T,N>`'s lane count is a compile-time integer literal, and **class**-level non-type
(const) generic parameters already ship (`CooperativeMatrix<T, uint32 Rows, uint32 Cols,
uint32 Use>`; validated by `TemplateInstantiator` with `CAJETA_ERROR_TYPE_PARAMETER_KIND`).
But **method**-level const generics do not: the grammar parses `<uint32 N>` on a method, yet
`MethodTemplateInstantiator` has no parameter-kind handling or const-argument substitution.
The width-generic `vload<N>` needs them.

### 2.1 Use cases
- 2.1.1 As a developer, I declare `public <uint32 N> Vector<T,N> vload(int32 i)` on a generic
  class and call `buf.vload<8>(i)`; `N` binds to the integer constant `8`.
- 2.1.2 As a developer, when I pass a non-constant or wrong-kind argument to a method const
  parameter, I get a clear error mirroring the class-level kind check
  (`CAJETA_ERROR_TYPE_PARAMETER_KIND`), not a silent miscompile.
- 2.1.3 As a developer, the const parameter `N` is usable in the method's **return type** and
  **body** (e.g. to size a `Vector<T,N>` or as a loop bound).
- 2.1.4 As a developer, two calls with the same `(method, type-args including const values)`
  resolve to one cached instantiation, exactly as class templates are keyed.
- 2.1.5 As a developer, a method may mix a type parameter and a const parameter
  (`<R, uint32 N>`), and const parameters compose with the class's own type parameter `T`.

---

## 3. Vectorized load — `buf.vload<N>(i)`

### 3.1 Use cases
- 3.1.1 As a kernel author, `buf.vload<N>(i)` returns a `Vector<T,N>` holding the N contiguous
  elements `buf[i] .. buf[i+N-1]`, where `T` is the buffer's element type and `N` the const arg.
- 3.1.2 As a kernel author, the load addresses memory through the same per-backend seam as
  `buf[i]` (raw pointer on CPU, descriptor on GPU) — no host array header is assumed.
- 3.1.3 As a kernel author, the load is a single packed operation where the backend supports
  width N natively, and a recombined set of native-width ops where it does not (§6) — the
  returned `Vector<T,N>` is identical either way.
- 3.1.4 As a kernel author, using `vload` **outside** an `@Kernel` body is a compile error with
  a message saying it is a kernel-only operation (device memory has no host address).
- 3.1.5 As a kernel author, the access is contiguous and the base index is in element units
  (not bytes); in-bounds is my responsibility on the performance path, consistent with how
  kernel buffer indexing already behaves (no implicit per-lane bounds check).

## 4. Vectorized store — `buf.vstore(i, v)`

### 4.1 Use cases
- 4.1.1 As a kernel author, `buf.vstore(i, v)` writes the N lanes of `v : Vector<T,N>` to the N
  contiguous elements starting at element index `i`. N is inferred from `v` — no const-generic
  argument is needed on store.
- 4.1.2 As a kernel author, the store uses the same addressing seam and the same native-width
  splitting rules as the load (§6), symmetrically.
- 4.1.3 As a kernel author, `vstore`'s element type must match the buffer's `T` and `v`'s lane
  type; a mismatch is a compile error.
- 4.1.4 As a kernel author, `vstore` outside an `@Kernel` body is the same compile error as 3.1.4.

## 5. Element-type & lane-width coverage

### 5.1 Use cases
- 5.1.1 As a kernel author, `vload`/`vstore` work for **floating** element types `float32` and
  `float64`.
- 5.1.2 As a kernel author, they work for **integer** element types (`int8/16/32/64` and the
  unsigned forms) — any `T` for which `Vector<T,N>` is a valid type.
- 5.1.3 As a kernel author, they work for every lane width `N` that `Vector<T,N>` permits
  (`2,4,8,16,...`); widths beyond a backend's native vector width are split (§6), never rejected.
- 5.1.4 As a kernel author, an unsupported `(T,N)` combination (one `Vector<T,N>` itself rejects)
  fails at compile time with the `Vector` type's existing diagnostic, not at lowering.

## 6. Backend lowering

### 6.1 Use cases
- 6.1.1 As a developer on the **CPU** backend, `vload<N>`/`vstore` lower to a packed `<N x T>`
  load/store (unaligned, `align 1`) through `bufferElementPtr`; LLVM legalizes to AVX/AVX-512.
- 6.1.2 As a developer on the **NVPTX** backend, they lower to native wide loads
  (`ld.global.v{2,4}` class) — N split into ⌈N/4⌉ (or ⌈N/2⌉) native ops and recombined.
- 6.1.3 As a developer on the **AMD** backend, they lower to native global vector loads/stores,
  split to the native width and recombined.
- 6.1.4 As a developer on the **Vulkan/SPIR-V** backend, because `OpTypeVector` is capped at 4
  components under the compute capabilities the triple pins, N>4 splits into ⌈N/4⌉ ≤4-wide
  loads (and the matching component stores), recombined into the `Vector<T,N>` SSA value.
- 6.1.5 As a compiler engineer, splitting+recombining is **shared** code; each backend supplies
  only its native addressing + packed-access primitive via a new lowering-target seam
  (`vectorLoad` / `vectorStore`), so a new backend implements one small hook.

## 7. (Optional, separable) Host-array parity

### 7.1 Use cases
- 7.1.1 As a developer, the same width-generic surface exists on host arrays
  (`T[].vload<N>(i)` / `arr.vstore(i, v)`), superseding the fixed-name `Cajeta.vload8f64` /
  `vstore8f64` intrinsics with one uniform, type-and-width-generic API; the legacy intrinsics
  remain as deprecated aliases.
- 7.1.2 As a developer, host-array and kernel-buffer vectorized access read identically in
  source — only the receiver type differs — so moving a numeric loop between host and kernel
  needs no rewrite of the load/store calls.

## 8. Acceptance themes
- 8.1 Method-level const generics: `<uint32 N>` methods instantiate, bind `N` as a constant,
  reject wrong-kind args, and cache by const value (§2).
- 8.2 `buf.vload<N>(i)` / `buf.vstore(i,v)` produce a single packed (or correctly recombined)
  vector op against device memory inside an `@Kernel`, for float + integer T and widths 2-16,
  and are a compile error outside a kernel (§3, §4, §5).
- 8.3 The same kernel source vectorizes and runs correctly on CPU, and lowers (with native-width
  splitting) on Vulkan, AMD, and NVPTX (§6).
- 8.4 The `samples/matmul-kernel` CPU kernel re-ported to `buf.vload8`/`vstore` vectorizes; the
  parallel run measurably approaches or beats numpy's 0.079 ms (n=200) — the SIMD x parallel
  product the scalar kernel could not realize (§1.2).
