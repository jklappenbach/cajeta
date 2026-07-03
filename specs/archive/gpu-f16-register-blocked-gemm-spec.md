# GPU f16 Register-Blocked WMMA GEMM — Spec

## 1. Definition

### 1.1 Purpose
Close the f16 WMMA GEMM performance gap against torch/rocBLAS on gfx1151 by adopting
the **register-blocked, deep-K tiling** technique their kernel uses — raising the
matmul-f16 bench from ~21 TFLOP/s (~54% of torch). The lever is kernel *structure*,
applied in portable `@Kernel` source, with AMD-deep codegen where it pays.

**Tiered ambition (not a flat 70%):** floor **~70%** of torch; **target parity, ~90–100%**;
**stretch: beat torch (>39 TFLOP/s)**. Parity is credible because cajeta's fp64 GEMM is
already 94–102% of torch *through the same `@Kernel`→LLVM→AMD codegen path* — so the path
is not the limiter, structure is; once the f16 structure is right there is no fundamental
reason to cap at 70%. Beating torch is on the table because torch's gfx1151 f16 (~39
TFLOP/s) sits at only ~70% of the ~50–59 TFLOP/s hardware peak and AMD's gfx1151 tuning is
still actively maturing upstream — and cajeta already beats torch on fp64. The one
structural risk is that Tensile hand-schedules its assembly inner loop while we rely on the
LLVM MachineScheduler; fp64 parity suggests LLVM suffices *given enough ILP* (which 12
accumulators provide), but this is the variable that decides parity vs. ~85%. The probe
(§6) and the unit measurements tell us which tier is real; we set ambition high and record
the honest residual rather than capping on paper.

### 1.2 Problem & evidence (reverse-engineered, 2026-06-27)
torch's `a @ b` dispatches one hipBLASLt → Tensile assembly kernel, tuned for gfx1151,
named `Cijk_..._HHS_..._MT96x128x64_MI16x16x1_..._MIWT3_4_..._WG32_4_1_..._PGR2_LRVW16_
GRVWA8_GRVWB8_DTLB0_...ISA1151`. Decoded against the Tensile generator source
(`~/code/ml/TheRock/rocm-libraries/shared/tensile`), the gap is **entirely kernel
structure, not hardware**:

| dimension | torch (Tensile) | cajeta `gemmF16` (variant B) |
|---|---|---|
| accumulators / wave (MIWT) | **3×4 = 12** | 2×2 = 4 |
| K-unroll (depthU) | **64** | 16 |
| LDS fragment read width | **256-bit, padded, conflict-free** | A 128-bit, B strided 16-bit |
| global→LDS path | VGPR-staged (`DTLB0`) | VGPR-staged — **identical** |
| measured @ n2048 | ~39 TFLOP/s | ~21 TFLOP/s (~54%) |

`DTLB0` is decisive: torch stages global→VGPR→LDS exactly as we do — **no async
global→LDS DMA** (gfx1151 lacks `global_load_lds`; that is a gfx1250 TDM feature). Its
speed comes from (a) 12 accumulators/wave reusing each loaded fragment across 3–4 WMMA
issues (high arithmetic intensity), (b) deep-K amortizing barriers, (c) padded LDS for
wide conflict-free reads, and (d) enough independent WMMA work in flight to hide LDS
latency. The HEAD scan of upstream confirmed no newer gfx1151 algorithmic lever (newer
wins are gfx1250 TDM/subtile hardware we do not have). This also explains the prior
`xpu-kernel-scheduling-hints` U4 NO-GO: manual scheduling could not help because our
4-accumulator kernel had too little parallel work to schedule — the fix is *creating*
the parallel work, not directing it.

### 1.3 Scope
- The `samples/profile` f16 WMMA GEMM kernel (`gemmF16`), measured on gfx1151 HIP.
- **AMD is the first-class target and we go as deep as the performance demands.** The
  author-facing kernel uses the portable `@Kernel` surface (`CooperativeMatrix`,
  `Shared`), but lowering/codegen is **segmented per vendor by design** — the existing
  `LoweringTarget` seam (AMD / NVPTX / SPIR-V-Vulkan / CPU, and Metal/Apple on the same
  pattern). So AMD-specific register-blocking codegen, wide padded-LDS emission, and
  AMD WMMA specifics are all in bounds. *(Decision A — RESOLVED: vendor-deep, not
  portable-only.)*
- **Componentize what is common, specialize what is not.** Logic shared across vendors
  (e.g. the register-blocking IR pattern, the padded-LDS index math) is factored into
  reusable components on the seam; anything vendor-specific specializes freely in that
  vendor's `LoweringTarget`. We do not hold AMD back to the portable subset, and we do
  not duplicate what every backend would share.
- **Compiler / codegen work is in scope but conditional** — added where a feasibility
  probe shows the structure cannot be expressed without pathological register spill or
  missing wide-LDS emission (§6). The developer is explicitly open to compiler/IR work,
  AMD-deep where it pays.

### 1.4 Constraints
- 1.4.1 **Correctness-preserving.** `check_ok=true` at every swept size (A=identity ⇒
  C==B), on every backend the kernel builds for.
- 1.4.2 **Measure-ship-record, rocprof-backed.** Every shipped gain is backed by an
  on-device min_ns→TFLOP/s A/B vs the variant-B baseline *and* a rocprof reading of the
  binding constraint (WMMA utilization / busy-cycles / occupancy), not an ISA guess.
- 1.4.3 **Honest target, not parity.** The goal is ~70% of torch at n2048 — a large,
  closable engineering gain — not full rocBLAS parity (years of accumulated tuning).

### 1.5 Non-goals
- 1.5.1 Full rocBLAS/Tensile parity, an autotuner, or a multi-solution selection library.
- 1.5.2 gfx1250 TDM / async global→LDS DMA / subtile / cluster-barrier / StreamK features
  (hardware we lack; the gfx1151 path stays VGPR-staged).
- 1.5.3 Precisions other than f16 (bf16/int8 WMMA reuse the structure — a named follow-on,
  `[[bf16-wmma-bench]]` / `[[int8-wmma-bench]]`, out of scope here).
- 1.5.4 A general register-blocking autotuner inside cajeta — we pick one good tuned shape
  (informed by torch's MT96x128x64/MIWT3×4), not a search.

---

## 2. Register blocking — the dominant lever

The kernel must hold a **wave-tile of N WMMA accumulators** (target ~8–12, e.g. 3×4)
such that each A and B fragment loaded from LDS is reused across multiple WMMA issues,
raising arithmetic intensity and creating enough independent matrix work to hide LDS
latency.

- 2.1 *As the kernel author, when I declare an M×N grid of `CooperativeMatrix<f32>`
  accumulators (e.g. 3×4) and issue WMMA across them reusing the loaded A/B fragments,
  then* the f16 GEMM keeps the matrix cores fed and arithmetic intensity rises vs the
  2×2 baseline.
- 2.2 *As the compiler, when a `@Kernel` declares ~12 live `CooperativeMatrix`
  accumulator fragments in a wave, then* it must register-allocate them without
  pathological spill (occupancy may drop — that is the intended latency-for-ILP trade,
  consistent with the variant-B finding). **This is the pivotal feasibility question
  (§6, Plan U1).**
- 2.3 *As the perf engineer, when I sweep the wave-tile shape (2×2 → 3×4 → larger), then*
  I can measure the arithmetic-intensity vs occupancy trade and pick the shape that
  maximizes measured TFLOP/s.

---

## 3. Deep K-unroll

- 3.1 *As the kernel author, when I deepen the K-panel (depthU 16 → 32 → 64) staged in
  LDS, then* each workgroup barrier is amortized over more WMMA compute and the inner
  loop issues many WMMAs per barrier.
- 3.2 *As the kernel, when depthU grows, then* the LDS panel allocation grows but must
  stay within the gfx1151 64 KB/workgroup budget (double-buffered) — the spec requires
  the chosen depthU × tile to fit with room for the accumulators.
- 3.3 *As the perf engineer, when I sweep depthU, then* I can measure the
  barrier-amortization gain and pick the depth that maximizes TFLOP/s without spilling.

---

## 4. LDS layout — padded, conflict-free wide reads

- 4.1 *As the kernel, when A and B tiles are stored in LDS with a **padded row stride**
  (extra elements per row to break 32-bank periodicity), then* the WMMA fragment loads
  issue as wide (256-bit / `ds_read_b128`+) conflict-free reads instead of the current
  strided 16-bit B reads. **Padding, not XOR swizzle** — Tensile uses padding, and our
  prior `Swizzled<T,S>` XOR regressed on f16 (`[[project_gpu_f16_wmma_ceiling]]`).
- 4.2 *As the kernel author, when I allocate a `Shared` tile with a padded stride and
  index with the call-site stride idiom, then* both the global→LDS write and the WMMA
  fragment read use the padded layout consistently. (Open: whether padding needs any new
  primitive or is just a wider `Shared` + stride arithmetic — §6 / Plan resolves.)
- 4.3 *As the perf engineer, when I A/B the padded layout, then* rocprof shows the
  fragment reads widened and bank-conflict / LDS-wait pressure dropped, with TFLOP/s up.

---

## 5. WMMA / LDS read-ahead interleave

- 5.1 *As the kernel, when the inner loop issues the next K-slice's `ds_read`s ahead of
  their consuming WMMAs with enough independent WMMA work between, then* the LDS-read
  latency overlaps matrix-core compute and the cores do not stall.
- 5.2 **The fix is providing enough parallel work, not manual scheduling hints.** The
  `cajeta.xpu.Schedule` intrinsics (shipped, `[[project_gpu_f16_wmma_ceiling]]`) were a
  NO-GO precisely because the 4-accumulator kernel had too little ILP; with 12
  accumulators the LLVM MachineScheduler has the material to interleave. The spec relies
  on the scheduler given sufficient ILP; manual `Schedule.*` hints are an optional last
  resort to be re-measured *after* register blocking, not a primary lever.

---

## 6. Compiler / codegen support (conditional on the §2 feasibility probe)

The spec mandates a **feasibility probe first** (Plan Unit 1): declare an N-accumulator
wave-tile + deep-K + padded LDS in a `@Kernel`, emit gfx1151 ISA, read VGPR count and
spill. The probe's outcome routes the rest of the work:

- 6.1 *When the probe shows the kernel structure compiles without pathological spill and
  with wide LDS reads, then* the remaining work is **kernel-only** (§2–§5 in `@Kernel`
  source) — the fastest path.
- 6.2 *When the probe shows pathological spill (the compiler cannot hold ~12 live
  accumulator fragments) or narrow/strided LDS emission, then* the work includes
  **compiler/codegen support** — register-allocation handling for many live cooperative-
  matrix fragments, and/or wide padded-LDS read/write emission, and/or `CooperativeMatrix`
  ergonomics for declaring a tile-grid. Each such gap is its own plan unit, TDD-first
  (GPU-free ISA/VGPR assertions), gated on the probe.
- 6.3 *As the developer, when compiler work is required, then* it follows the segmented
  seam: **AMD-deep where the perf demands it** (this is the measured target), with any
  logic common across vendors **componentized on the `LoweringTarget` seam and reused**
  (so a register-blocked WMMA kernel benefits NVPTX/SPIR-V/Metal too where they share the
  pattern). Not a one-off bench hack, and not held back to the portable subset —
  consistent with "componentize the common, go deep per vendor."

---

## 7. Measurement & acceptance

- 7.1 *As the perf engineer, when I measure the redesigned kernel, then* I report min_ns →
  TFLOP/s at n256–n2048 on gfx1151 HIP, A/B vs the variant-B baseline (818593 ns / 21.0
  TFLOP/s @ n2048) **and** vs torch (~39 TFLOP/s, re-measured contemporaneously), all
  `check_ok=true`.
- 7.2 *When a redesigned variant beats variant B, then* it is shipped as the bench kernel
  with the rocprof evidence (WMMA utilization / busy-cycles / occupancy showing the
  binding constraint moved).
- 7.3 **Acceptance:** a measured A/B + rocprof table; the shipped kernel beats variant B
  at n2048 with `check_ok=true` at all sizes. **Tiered target:** floor ~70% of torch
  (~27 TFLOP/s); target **parity, ~90–100%** (~35–39 TFLOP/s — credible per the fp64
  precedent, §1.1); stretch **beat torch (>39 TFLOP/s)**. We push for the highest tier the
  measurements support and record the honest residual at whatever tier we land. The
  comparison is apples-to-apples: torch's kernel is `HHS` (f16 in, **f32 accumulate**), no
  split-K — the same precision as our WMMA. Memory updated (`[[project_gpu_f16_wmma_ceiling]]`).
