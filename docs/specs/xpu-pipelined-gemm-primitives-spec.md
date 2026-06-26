# XPU Pipelined-GEMM Primitives — Spec

## 1. Definition

### 1.1 Purpose
Add the two compiler/language primitives that let a Cajeta `@Kernel` GEMM reach
**vendor-library parity** on the GPU matrix cores: (a) an **async global→shared copy**
that enables deep software pipelining, and (b) a **swizzled shared-memory layout** that
eliminates LDS bank conflicts on the cooperative-matrix fragment loads. Together they
close the documented f16 WMMA gap (Cajeta ~15 TFLOP/s vs PyTorch/rocBLAS ~25–41 TFLOP/s
on gfx1151 — `project_gpu_matmul_tiling_crown`, plan `gpu-matmul-tiling` U3).

### 1.2 Problem
The shipped double-buffered f16 WMMA kernel plateaus at ~58% of torch f16. The remaining
gap is **not** kernel-structure (measured: deeper-K hurts, bigger tiles spill, fragment-
load instruction count is not the bottleneck — transposing B to vectorize its load fixed
the ISA but regressed on bank conflicts). The gap is the **pipelining + swizzle layer**
that hand-tuned Composable Kernel uses and that Cajeta `@Kernel` source cannot currently
express:
- Staging today goes **global → registers → LDS** (two hops, register pressure, a barrier
  on the critical path). Vendors issue a **direct global→LDS async copy** that retires off
  the compute path, enabling an N-stage prefetch pipeline (not just 2-buffer ping-pong).
- LDS today is addressed **linearly**, so the WMMA fragment loads hit **bank conflicts**;
  vendors **XOR-swizzle** the LDS address so each lane hits a distinct bank.

### 1.3 Scope
Two device-only primitives usable inside `@Kernel`/`@Device`, lowered per backend
(AMDGPU first — the parity target; NVPTX + SPIR-V to follow the same surface):
1. `AsyncCopy` — issue, group-commit, and wait on direct global→`Shared<T>` transfers.
2. `Shared<T>` **swizzle** — an opt-in conflict-free LDS addressing mode.

### 1.4 Constraints
- Pure additive surface over the existing `Shared` / `Barrier` / `CooperativeMatrix` /
  `CoopStage` model ([[feedback_one_unified_allocation_borrow_model]] — one allocation/
  borrow model; no special-case bypass). Existing kernels compile and run unchanged.
- AMDGPU lowering uses the native global→LDS path (`buffer_load_*` with the LDS bit /
  `global_load_lds` per gfx1151 ISA) + `s_waitcnt vmcnt`; NVPTX uses `cp.async` +
  `cp.async.commit_group` / `cp.async.wait_group`; SPIR-V uses
  `OpCooperativeMatrixLoad`'s async/`MemoryAccess` where available, else a correct
  synchronous fallback (the primitive degrades to today's staged copy, never miscompiles).
- Swizzle must be expressible such that BOTH the staging store and the `CooperativeMatrix`
  fragment load agree on the permuted address (consistency is a type/whole-tile property,
  not a per-access flag the user can mismatch).

### 1.5 Non-goals
- Not a managed/auto-tuned GEMM op (the `@Kernel` author still writes the loop). A fully-
  managed tiled-GEMM block is a possible later convenience, out of scope here.
- Not split-K / stream-K reduction schemes (a separate follow-up if parity needs them).
- Not the Vulkan f64 correctness work ([[reference_spirv_8wide_f64_legalize_fail]]).

---

## 2. Async global→shared copy (`AsyncCopy`)

A device-only primitive that issues a direct global→`Shared<T>` transfer that does not
pass through registers and retires asynchronously, plus group-commit/wait so the kernel
can run an N-stage prefetch pipeline.

Proposed surface (final naming in the plan):
```
AsyncCopy.copy(Shared<T> dst, uint32 dstOffset, KernelBuffer<T> src, uint32 srcOffset, uint32 count);
AsyncCopy.commit();                 // close the current copy group
AsyncCopy.wait(uint32 groupsInFlight);  // wait until <= N groups remain outstanding
```

### 2.1 Use cases
- 2.1.1 As a kernel author, when I stage a K-panel, I issue `AsyncCopy.copy` per thread-
  tile instead of `global load → sa[...] = reg`, so the transfer retires off the compute
  path and no VGPRs are tied up holding staged data.
- 2.1.2 As a kernel author, when I want an N-stage pipeline, I prefetch panels `k+1..k+N`
  with N copy groups, then each K-step `AsyncCopy.wait(N-1)` + `Barrier.workgroup()` before
  consuming the oldest staged panel — so global latency is hidden behind N-1 panels of
  compute (today's double-buffer is the N=2 special case).
- 2.1.3 As the AMDGPU backend, when I lower `AsyncCopy.copy`, I emit the native global→LDS
  load (the LDS-direct `buffer_load`/`global_load_lds` form) and track it under `vmcnt`;
  `wait(n)` lowers to the matching `s_waitcnt vmcnt(n)`.
- 2.1.4 As the NVPTX backend, I lower `copy`→`cp.async.ca.shared.global`, `commit`→
  `cp.async.commit_group`, `wait(n)`→`cp.async.wait_group n`.
- 2.1.5 As a backend with no async path (CPU emulation, or a SPIR-V env lacking it), I
  lower `copy` to the existing synchronous strided staging copy and `commit`/`wait` to
  no-ops — bit-identical result, just no overlap (the `note:` tiering pattern from
  `CooperativeMatrix` applies: disclose the synchronous fallback).
- 2.1.6 As a kernel author, `AsyncCopy` composes with `Barrier.workgroup()` for the
  cross-wave visibility rendezvous exactly as manual staging does today (the primitive
  orders the transfer; the barrier publishes it to other waves).

---

## 3. Swizzled shared memory

An opt-in conflict-free addressing mode for a `Shared<T>` tile so that the per-lane WMMA
fragment loads (and the staging stores) map to distinct LDS banks.

Proposed surface: a swizzle is a property of the shared tile, applied consistently to
every access, e.g. `Shared<float16> sa = shared swizzled float16[2 * 2048];` (final form
in the plan — keyword vs. typed wrapper is a design item), with the compiler applying the
same XOR-permutation in both `sa[i] = …` stores and `CooperativeMatrix.load(sa, …)`.

### 3.1 Use cases
- 3.1.1 As a kernel author, when I declare a staging tile `swizzled`, the WMMA fragment
  loads that today emit bank-conflicting `ds_load`s instead hit one bank per lane — without
  my hand-transposing the data (which I measured to regress via different conflicts).
- 3.1.2 As the compiler, when a `Shared<T>` is swizzled, I apply the SAME address
  permutation to every access of that tile — the staging store, any direct `sa[i]`, and the
  `CooperativeMatrix` fragment load — so the permutation is invisible to kernel logic and
  cannot be mismatched between producer and consumer.
- 3.1.3 As the AMDGPU backend, I lower the swizzle to the standard XOR pattern
  (`addr ^ ((addr >> c) & mask)` sized to the bank width / element type) proven to make
  16×16 f16 fragment access conflict-free on RDNA3 LDS.
- 3.1.4 As a backend where swizzle is unnecessary or unsupported, the swizzle is the
  identity permutation (correct, unoptimized) — kernels stay portable.
- 3.1.5 As a kernel author, a swizzled tile interoperates with `AsyncCopy` (§2): the async
  copy writes into the swizzled tile through the same permutation, so a swizzled, async-
  staged, N-stage-pipelined WMMA GEMM is expressible end-to-end.

---

## 4. Acceptance / fidelity (the parity bar)

- 4.1 The f16 `matmul-f16` bench, rewritten on `AsyncCopy` (N≥3 stages) + swizzled LDS,
  is **≥ PyTorch f16** at n=2048 on gfx1151 (the size where torch is near peak), or the
  spec records the measured residual and the next-identified primitive — whichever is true.
- 4.2 All existing XPU device tests + the gpu-area benches stay `check=true` (the
  primitives are additive; a kernel not using them is byte-identical).
- 4.3 Each primitive has the documented synchronous/identity fallback so the SAME kernel
  still runs (un-accelerated) on backends without the native path, with a `note:` tier
  disclosure.

## 5. Design decisions (resolved 2026-06-26, approved)
- 5.1 **Swizzle surface = a `Swizzled<T>` typed tile** (not a bare keyword): the
  permutation is a property of the tile's TYPE, so producer (store) and consumer
  (`CooperativeMatrix.load`) provably agree — a mismatch is a compile error, consistent
  with the type-carried tier rules on `CooperativeMatrix`. Declared
  `Swizzled<float16> sa = shared swizzled float16[N];` (the tile type is `Swizzled<T>`).
- 5.2 **`AsyncCopy` exposes both**: the low-level `copy/commit/wait` verbs (§2) AND a
  `CoopStage.panelAsync(dst, src, …)` panel helper layered on top (the async analog of
  `CoopStage.panel`) — authors reach for the helper; the verbs stay for custom patterns.
- 5.3 **N-stage buffering is author-managed** (no managed ring-buffer in v1): the author
  declares an N-deep `Shared`/`Swizzled` tile and indexes the stage, exactly as the
  shipped double-buffer hand-rolls 2 stages. A managed ring-buffer is a later convenience
  (non-goal, §1.5).
