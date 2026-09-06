# Spec: Parallel scan (prefix-sum) primitive (`xpu-scan-primitive`)

## 1. Definition

### 1.1 Purpose

Add **parallel scan (all-prefix-sums)** to the XPU kernel library as the first
foundational primitive from the `xpu-kernel-library` backlog (its gap catalog is
§10 of the archived [`xpu-kernel-scheduling`](archive/xpu-kernel-scheduling-spec.md);
the kernel families each workload needs are now listed without status in
[`xpu-tile-workload-profiles`](xpu-tile-workload-profiles-spec.md) §5.2). Scan is the **root dependency** of the backlog: scatter-compaction,
radix sort, histogram, sparse-format (CSR) construction, GPU-driven culling / LOD
selection (gfx), and continuous-batching offset computation (LLM) all reduce to a
scan. Building it unblocks the rest.

Given input `[a0, a1, ..., a_{n-1}]` and an associative operator `⊕` with identity
`e`, produce:
- **exclusive**: `[e, a0, a0⊕a1, ..., a0⊕...⊕a_{n-2}]`
- **inclusive**: `[a0, a0⊕a1, ..., a0⊕...⊕a_{n-1}]`

### 1.2 Scope

- A device-wide scan over a `KernelBuffer<T>`: **inclusive + exclusive**, for
  `u32`/`i32`/`f32` first, over operators `+` (primary), `max`, `min`.
- The standard **three-level composition**: **wave/subgroup** scan → **workgroup**
  scan → **device** scan.
- Two device-level strategies with a portability switch: **single-pass decoupled
  look-back** (fast) and **two-pass reduce-then-scan** (portable fallback).
- **Segmented scan** (ragged/per-segment) as a follow-on tier.
- A `@Kernel` surface + host wrapper mirroring the existing `reduceSumF32` /
  `matmulF32` launch pattern (`KernelStream.launch(...)`).

### 1.3 Non-goals

- Not a generic user-defined-functor scan in v1 (fixed operator set first;
  functor generality later).
- Not multi-GPU scan (single device).
- Not the consumers themselves (scatter/compaction/sort ship on top, separately).

### 1.4 Principles

- **Build on what exists.** `cajeta.xpu.Wave` already exposes the hard,
  backend-specific piece — `shuffleSync`, `ballotSync`, `reduceSum`, and a
  **wave-level exclusive prefix-sum** (lowered per backend). Scan composes the
  workgroup and device levels on top of it + `Shared` (LDS) + `Barrier` + atomics.
- **Work-efficient.** O(n) work (Blelloch up-sweep/down-sweep), not O(n log n);
  bandwidth-bound, so minimize global passes.
- **Portable-by-design, with a fast path.** Decoupled look-back is fastest but
  assumes forward-progress + ordered atomics that not every backend guarantees;
  the two-pass path is the always-correct fallback. Same tiered-degradation
  discipline as the shipped XPU specs — never a hard error on a weaker backend.
- **Correctness-preserving + measured.** Results match a serial reference at every
  size on every backend; any performance claim is on-device (rocprof/CUPTI), and
  the kernel carries the roofline-class + footprint metadata the scheduler reads
  (**memory-bound**).

## 2. Three-level composition

### 2.1 Requirement

Scan an arbitrarily long buffer by composing scans at wave, workgroup, and device
granularity.

### 2.2 Mechanism

1. **Wave scan** — `Wave` exclusive prefix over the lanes of one wave/subgroup
   (have it); the wave's total is its last lane's inclusive value.
2. **Workgroup scan** — each wave writes its total to `Shared` (LDS); one wave
   scans the per-wave totals; each lane adds its wave's exclusive prefix.
   `Barrier` between phases. (Harris GPU-Gems hierarchy, bank-conflict-aware LDS.)
3. **Device scan** — combine per-workgroup totals into per-workgroup prefixes
   (§3), then each workgroup adds its prefix to its local scan.

### 2.3 Use cases

- A 10M-element exclusive scan runs as wave→block→device with two global reads
  (fast path) or three (two-pass).

## 3. Device-level strategy (the design decision)

### 3.1 Requirement

Turn per-workgroup totals into per-workgroup exclusive prefixes with minimal
global traffic, portably.

### 3.2 Mechanism — two strategies behind one switch

- **Two-pass reduce-then-scan (portable, default).** Pass A: each workgroup writes
  its total. Host/second kernel scans the (small) array of totals. Pass B: each
  workgroup adds its prefix. Simple, correct on every backend; two extra global
  touches.
- **Single-pass decoupled look-back (fast path).** Each workgroup publishes its
  local aggregate, then looks back at predecessors' published
  aggregate/inclusive-prefix status flags, accumulating until it finds a completed
  inclusive prefix — overlapping local work with global propagation in one pass
  (Merrill & Garland; the CUB approach). Selected only where the backend
  guarantees the required forward progress + atomic ordering; otherwise fall back
  to two-pass.

The switch is a `DeviceProfile` capability query (does this backend/arch support
the look-back's memory model?) with the two-pass path as the safe default.

### 3.3 Use cases

- NVPTX/AMDGPU with the needed guarantees → single-pass; SPIR-V/CPU or unknown →
  two-pass. Both produce identical results.

## 4. Surface

### 4.1 Requirement

Expose scan as ordinary library kernels + host wrappers, consistent with the
existing `Ewise` primitives.

### 4.2 Mechanism

- Device kernels in a new `runtime/src/cajeta/math/Scan.cajeta`:
  `scanExclusiveF32` / `scanInclusiveF32` (and `...U32`), plus the internal
  block-aggregate/look-back kernels.
- Host wrapper on `Tensor<T>` / `KernelBuffer<T>` mirroring `sumF32`'s
  `KernelStream.launch(...)` + `sync()` pattern, choosing strategy from
  `DeviceProfile`.
- Operator variants via a small compile-time selector (`+`, `max`, `min`);
  generic functor deferred.

### 4.3 Use cases

- `scanExclusiveU32(offsets, counts, n)` builds insertion offsets for a
  subsequent scatter/compaction — the exact culling/binning/CSR-build use.

## 5. Segmented scan (follow-on tier)

- Per-segment scan with head flags (Sengupta), for ragged/batched inputs
  (LLM ragged batches, per-cluster gfx). Ships after the flat scan is solid.

## 6. What this unblocks

Directly enables the next backlog items, each a thin layer on scan:
- **scatter / compaction** — scan the predicate → exclusive offsets → scatter.
- **radix sort** — per-digit histogram + scan of bucket offsets.
- **histogram** — scan for bin offsets.
- **sparse (CSR) build** — row-pointer array is a scan of per-row counts.
- **GPU-driven culling / LOD selection** (gfx), **continuous-batching offsets**
  (LLM), **spatial-hash/voxel builds** (robotics).

## 7. Risks / dependencies

1. **Wave-prefix portability** — semantics/width of the wave op differ across
   NVPTX/AMDGPU/SPIR-V; verify and, where absent, synthesize from `shuffleSync`.
2. **Decoupled-look-back memory model** — forward-progress + atomic-ordering
   assumptions are not universal; gate behind `DeviceProfile`, default to
   two-pass.
3. **LDS bank conflicts** — the workgroup scan must pad/av oid conflicts (Harris).
4. **Metadata** — must publish roofline-class (memory-bound) + footprint for the
   scheduler; ties to `xpu-device-profile` / `kernel-occupancy-autotune`.

## 8. References

Corpus + markers in [`research/gpu-primitives/papers/`](../research/gpu-primitives/papers/):
Blelloch (work-efficient scan + applications), Harris GPU-Gems (GPU block/device
hierarchy + LDS bank conflicts), Sengupta (segmented scan primitives), Merrill &
Garland (single-pass decoupled look-back). Consumes `cajeta.xpu.Wave` /
`Workgroup` / `Shared` / `Barrier`; roofline/footprint via `xpu-device-profile` +
`kernel-occupancy-autotune`. Backlog + priority: local `xpu-kernel-library` plan;
gap catalog in the archived [`xpu-kernel-scheduling`](archive/xpu-kernel-scheduling-spec.md) §10.
