# GPU f16 Torch-Recipe GEMM — Spec

## 1. Definition

### 1.1 Purpose
Reach (or closely approach) PyTorch/hipBLASLt's **verified 38.2 TFLOP/s** f16 matmul on
gfx1151 (Strix Halo, RDNA3.5 WMMA) by implementing — in Cajeta `@Kernel` source plus a
small set of **general `cajeta.xpu` primitives** — the exact kernel recipe torch's
tensilelite solution uses on this device. The recipe is no longer inferred: it was read
off the live dispatched kernel (`torch.profiler`) and cross-checked against the
tensilelite solution parameters in `TheRock` (`ValidParameters.py`, `Naming.py`).

### 1.2 Problem (the verified recipe vs our gap)
Torch's dispatched kernel on gfx1151 is
`Cijk_..._MT96x128x64_MI16x16x1_..._GRVWA8_GRVWB8_..._LRVW16_..._MIWT3_4_..._PGR2_..._DTLB0_..._ISA1151_..._WG32_4_1`,
i.e. **96×128 tile, depthU 64, 12 accumulators/wave (MIWT 3×4), 4 waves / 128 threads
(wave32), f16/f16/f32 (HHS)** — the same structure we already build and run correctly. The
performance comes from four techniques our two naive attempts (11.3 and 5.1 TFLOP/s) each
only partially had:
- **Transposed + padded B LDS** (`TransposeLDS=1`, `LdsPadB=8`): B stored transposed so the
  K (summation) dim is LDS-contiguous → the WMMA fragment read is wide. The local *write*
  is then necessarily strided; torch does **not** make B wide-both-sides — it **hides** the
  strided write. (Not swizzle, not tile-major.)
- **Two-stage global prefetch** (`PrefetchGlobalRead=2`): per the source, "do another
  prefetch while writing data from vgpr to lds" — overlaps the vgpr→LDS commit of the
  current K-panel with the global→vgpr read of the next, on top of double LDS buffering.
  **This is the crux** and is exactly what our kernels lacked (naive double-buffer only).
- **Wide global loads on BOTH operands** (`GRVWA8`/`GRVWB8` = 128-bit): our wide-B variant
  loaded B with *scalar* global reads — a miss.
- **Wide grouped local reads** (`LRVW16`): for fp16, two `ds_read_b128` worth per fragment
  read group (the source caps a single b128 at LRVW 8), fetching 2 K-steps per group for
  better read scheduling.

Two prerequisites are already banked: vectorized LDS staging is kernel-only
(`vstore`→`ds_store_b128`), and device IR optimization now lets the 12-acc/depthU-64 kernel
fit registers (172/0, no spill). The wide-B *read* primitive (transposed col-major →
`ds_read_b128`) is also proven.

### 1.3 Scope
Build the four techniques, **the prefetch pipeline and transposed-B stage as general
`cajeta.xpu` primitives** (reusable by any WMMA GEMM, not bench-local), then integrate them
into the depthU-64/12-acc/96×128 f16 GEMM and measure on-device:
1. A **transposed + padded B staging** primitive (wide global load → strided transposed
   write into padded LDS, conflict-free).
2. A **two-stage (PGR2) global→vgpr→LDS prefetch pipeline** primitive (general, N-stage-
   capable; PGR2 the target depth).
3. **Wide grouped cooperative-matrix reads** (LRVW16-equivalent: a fragment read group
   fetching 2 K-steps), which may require a `coopMatrixLoad` change.
4. The **integrated torch-recipe f16 GEMM** kernel built from the above, replacing the
   bench kernel only if it wins.

### 1.4 Non-goals
- No async/DMA global→LDS (`global_load_lds` Cannot-selects on gfx1151; torch is `DTLB0`/
  VGPR-staged too — irrelevant here).
- No split-K / GlobalSplitU (torch is `GSU` off for this shape).
- No new tile geometry search — the geometry (96×128/12-acc/depthU-64/4-wave) is fixed by
  torch's verified config; this effort is about *execution quality*, not shape tuning.
- No multi-arch parameterization yet (CDNA/RDNA strategy table) — that is a separate effort;
  here we target gfx1151 RDNA3.5, but the primitives are written to be arch-portable.

### 1.5 Constraints
- **amdgpu-only flavor** for the bench (CPU barrier-fission OOMs on the K-loop barrier —
  `reference_cpu_barrier_fission_loop_oom`); GPU-free ISA/spill probes where possible, then
  on-device.
- On-device runs need a **quiet GPU** (coordinate with the developer).
- The new primitives are **additive** over the existing `cajeta.xpu` staging surface
  (`CoopStage`/`AsyncCopy`/`Swizzled`/`CooperativeMatrix`) and the one unified allocation/
  borrow model — no special-case bypass (`feedback_one_unified_allocation_borrow_model`).
- **Honest measurement**: every throughput claim is min-ns→TFLOP/s with the variant tag and
  an exact `check_ok` cross-check; ISA/VGPR claims come from a probe; ship-or-record.

### 1.6 Acceptance ladder (target tiers)
Measured at n=2048 on gfx1151, `check_ok=true` required at every tier:
- **Floor**: beat the committed variant B (> ~20 TFLOP/s).
- **Good**: ≥ 75% of torch (~28 TFLOP/s).
- **Stretch**: parity (~36–38 TFLOP/s).
The bench is switched to the new kernel only at the Floor tier or above; below it, variant B
stays and the residual + next lever are recorded.

## 2. Transposed + padded B staging primitive (verified: `TransposeLDS=1`, `LdsPadB=8`)

### 2.1 Requirements
A reusable primitive that stages a B K-panel global→LDS as **transposed + padded**: wide
128-bit global loads (`GRVWB8`) into registers, then a transposed write into a padded LDS
tile (`[N][Kpad]`) such that the subsequent WMMA fragment read is wide (`ds_read_b128`) and
bank-conflict-free. The pad amount is a parameter (torch uses 8 elements). The primitive
must compose with the prefetch pipeline (§3) — i.e. its global-read and LDS-write phases are
separable so the pipeline can overlap them.

### 2.2 Use cases
- **2.2.1** As a WMMA-GEMM author, when I stage a B panel through this primitive, then the
  global reads emit `global_load_b128` (wide) and the WMMA B fragment reads emit
  `ds_read_b128` (wide), verified by a GPU-free ISA probe.
- **2.2.2** As a WMMA-GEMM author, when the B tile is padded by the configured amount, then
  the transposed LDS access is bank-conflict-free (verified on-device via rocprof LDS
  conflict counter, or by a no-regression throughput check vs unpadded).
- **2.2.3** As a WMMA-GEMM author, when I use the primitive with f16/bf16/int8 element
  types, then it lowers correctly for each (generality, even if only f16 is benchmarked).
- **2.2.4** As a kernel, the transposed B result is **bit-identical** to a scalar
  transposed reference (CPU oracle / on-device `check_ok`).

## 3. Two-stage global prefetch pipeline primitive (verified: `PrefetchGlobalRead=2`)

### 3.1 Requirements
A general staging primitive that runs a **two-stage global→vgpr→LDS prefetch** over a K-loop
with double LDS buffering: while the current panel's vgpr→LDS commit is in flight, the next
panel's global→vgpr read is issued (the PGR2 overlap). It must be expressible from `@Kernel`
source, compose with the transposed-B stage (§2) and the cooperative-matrix compute, and not
spill at the recipe's register footprint (≤ the 256-VGPR budget; baseline depthU-64/12-acc is
172/0). The depth (1-stage / 2-stage) is a parameter; 2-stage is the target. PGR2 is the
crux technique.

### 3.2 Use cases
- **3.2.1** As a WMMA-GEMM author, when I drive the K-loop through the pipeline at depth 2,
  then for each iteration the next panel's global loads are issued before the current
  panel's WMMA, and the vgpr→LDS commit overlaps the next global read — confirmed by ISA
  inspection (global loads scheduled ahead of `ds_write`/WMMA) and on-device throughput.
- **3.2.2** As a WMMA-GEMM author, when the pipeline runs at the recipe footprint
  (12 accs, depthU 64), then a GPU-free spill probe shows `vgpr_spill_count == 0`.
- **3.2.3** As a WMMA-GEMM author, when I select pipeline depth 1 vs 2, then both lower and
  run correctly, so the per-stage contribution can be measured (depth is a parameter).
- **3.2.4** As a kernel, the pipelined result is **bit-identical** to the unpipelined
  reference (`check_ok`).

## 4. Wide grouped cooperative-matrix reads (verified: `LRVW16`)

### 4.1 Requirements
The WMMA B (and where applicable A) fragment read should fetch **multiple K-steps per read
group** (LRVW16 for fp16 = two `ds_read_b128` worth), reducing local-read instruction
dispatch pressure and improving scheduling. This may require a `coopMatrixLoad` change to
issue grouped/wider reads; it must remain correct and fall back cleanly where the wider read
cannot form. Bench impact is measured, not assumed.

### 4.2 Use cases
- **4.2.1** As a WMMA-GEMM author, when fragment reads are grouped, then a GPU-free ISA
  probe shows the per-K-step `ds_read` count drop (e.g. 2 K-steps fused per read group),
  with no `Cannot select` and no correctness change.
- **4.2.2** As a WMMA-GEMM author, when grouped reads are enabled vs disabled, then the
  on-device throughput delta is measured and reported (so the lever's value is known, not
  assumed); a kill-switch restores the per-step path.

## 5. Integrated torch-recipe f16 GEMM kernel

### 5.1 Requirements
A depthU-64 / 12-acc / 96×128 / 4-wave f16 WMMA GEMM that composes §2+§3+§4 (transposed-
padded B, PGR2 pipeline, wide global loads on A and B, grouped reads), bounds-checked on M
(96 ∤ n), built amdgpu-only. It must be correct on-device at all sizes and faster than
variant B (else not shipped).

### 5.2 Use cases
- **5.2.1** As the bench, when run on-device at n∈{256,512,1024,2048}, then `check_ok==true`
  for every size (A=identity ⇒ C==B exact cross-check).
- **5.2.2** As the bench at n=2048, then throughput is measured (min-ns→TFLOP/s) and reported
  against variant B (~20) and torch (38.2) with the variant tag.
- **5.2.3** As the developer, when the integrated kernel meets a tier (§1.6), then the bench
  is switched to it; otherwise variant B stays and the residual is recorded.

## 6. Verification, measurement & honesty

### 6.1 Requirements
GPU-free ISA/VGPR/spill probes gate each primitive before any device run (mirroring the
prior effort's probe-first discipline); correctness is the exact on-device `check_ok`;
throughput is min-ns→TFLOP/s with the variant tag; rocprof is used where a claim needs a
hardware counter (LDS bank conflicts, occupancy). No silent caps; ship-or-record.

### 6.2 Use cases
- **6.2.1** As a reviewer, when I read a unit's probe output, then I see the wide-op counts
  (`global_load_b128`, `ds_store_b128`, `ds_read_b128`) and `vgpr_count`/`vgpr_spill_count`
  for the kernel under test.
- **6.2.2** As the developer, when an integrated variant fails to beat variant B, then the
  bench stays variant B and the residual + the next candidate lever are recorded
  (`project_gpu_f16_wmma_ceiling` / focus stack).
- **6.2.3** As the developer, when a torch-recipe claim is made, it traces to either the live
  `torch.profiler` kernel name or the tensilelite source — not inference.
