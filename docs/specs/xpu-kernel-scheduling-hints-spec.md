# XPU Kernel Scheduling Hints — Spec

## 1. Definition

### 1.1 Purpose
Add a **portable, architecture-neutral `@Kernel`-surface for instruction-scheduling
hints** — the controls that let a kernel author steer how the backend interleaves
matrix-core, LDS, and global-memory instructions. The motivating problem is the f16
WMMA GEMM **scheduling ceiling**: `matmul-f16` plateaus at ~20.7 TFLOP/s = ~50% of
PyTorch f16 at n=2048, and the gap is *not* reachable by source-level kernel
restructuring (occupancy, LDS layout, swizzle, tiling were all exhausted —
`project_gpu_f16_wmma_ceiling`). The residual is the WMMA/memory **instruction
schedule** that rocBLAS/Composable Kernel hand-tune; AMD exposes it through scheduling
intrinsics that the Cajeta `@Kernel` surface currently cannot reach.

### 1.2 Scope
- A small device-intrinsic surface in `cajeta.xpu.Schedule`, usable inside any `@Kernel`.
- Lowering through the existing per-backend `LoweringTarget` seam: **AMD** emits the
  native scheduling intrinsics; **NVPTX / SPIR-V / CPU** degrade to documented no-ops.
- An **empirical** application to `gemmF16`, measured on-device against the shipped
  variant-B baseline with rocprof.

### 1.3 Constraints
- 1.3.1 **Portable-by-design.** The surface compiles on *every* backend (AMD lowered,
  others no-op) — never a hard error — the same pattern as the just-shipped
  `AsyncCopy`/`Swizzled` U5 portability work (`xpu-pipelined-gemm-primitives`).
- 1.3.2 **Correctness-preserving.** A scheduling hint is an *optimization directive*: it
  must never change kernel results. `check_ok=true` at every swept size, on every backend.
- 1.3.3 **rocprof-verified.** Any shipped win is backed by an on-device rocprof + TFLOP/s
  measurement (rocprofv3 1.1.0 on the 7.11 stack), not an ISA guess.

### 1.4 Non-goals
- 1.4.1 **No parity guarantee.** This is measure-ship-or-record-residual; closing the
  full 2× f16 gap is not promised (it may be partly hardware-bound — gfx1151 lacks
  `global_load_lds` for async pipelines, `reference_gfx1151_no_vmem_to_lds`).
- 1.4.2 **No automatic scheduling.** The compiler does not *infer* hints; the author
  places them. (Auto-insertion is a possible future effort, out of scope here.)
- 1.4.3 **No new backend scheduler.** We expose existing LLVM AMDGPU intrinsics, not a
  custom instruction scheduler.

---

## 2. The `cajeta.xpu.Schedule` surface

Body-level device intrinsics (empty-bodied, call-site-lowered like `AsyncCopy`/`CoopStage`
— NOT `@Native`), placed in the kernel instruction stream where the hint applies. Four
controls, each a thin wrapper over an AMD scheduling intrinsic, each with constant
(`ImmArg`) operands where the intrinsic requires them.

### 2.1 Use cases
- 2.1.1 *As a kernel author, when I place `Schedule.barrier(mask)` between two regions,
  then* the backend must not move instructions of the masked classes across that point
  (a one-sided scheduling fence) → AMD `llvm.amdgcn.sched.barrier(mask)`.
- 2.1.2 *When I emit `Schedule.groupBarrier(mask, size, syncId)` in a sequence, then* the
  backend assembles an explicit instruction group of `size` instructions matching `mask`,
  synchronized with other group-barriers sharing `syncId` — letting me hand-build a
  WMMA/DS/VMEM interleave → AMD `llvm.amdgcn.sched.group.barrier(mask, size, syncId)`.
- 2.1.3 *When I call `Schedule.priority(level)`, then* the issuing wave's scheduling
  priority is set to `level` (0–3), so latency-critical waves win issue slots → AMD
  `llvm.amdgcn.s.setprio(level)`.
- 2.1.4 *When I call `Schedule.pipelineOpt(strategy)` once at kernel entry, then* the
  backend applies its canned pipeline schedule `strategy` (0 = GEMM-style matrix/LDS
  interleave) → AMD `llvm.amdgcn.iglp.opt(strategy)`. **Documented caveat:** the canned
  strategies are MFMA/CDNA-tuned; on RDNA WMMA this may select nothing (an effective
  no-op) — that outcome is recorded, not an error.

### 2.2 Operand validation
- 2.2.1 *When a control's operand must be a compile-time constant (`mask`, `size`,
  `syncId`, `strategy`, `level` are all `ImmArg`) and the author passes a non-constant,
  then* the compiler reports a clear diagnostic at the call site (not an LLVM verifier
  crash).
- 2.2.2 *When the author passes an out-of-range `level` (not 0–3) or unknown `strategy`,
  then* the compiler reports a clear diagnostic.

---

## 3. AMD lowering

### 3.1 Use cases
- 3.1.1 *As the AMDGPU backend, when a kernel uses any `Schedule.*` control, then* the
  emitted ISA contains the corresponding instruction (`s_sched_barrier` /
  `s_sched_group_barrier` / `s_setprio` / the iglp pseudo) — asserted GPU-free via the
  ISA-emit test path (the `emitIsaForArch` pattern).
- 3.1.2 *When a `Schedule.*` control is lowered, then* it compiles clean on gfx1151 (no
  Cannot-select, no verifier error) — these intrinsics are not target-gated.
- 3.1.3 *When a kernel uses `Schedule.*` controls, then* its numerical result is
  unchanged vs. the same kernel without them (`check_ok=true` on-device).

---

## 4. Portability (NVPTX / SPIR-V / CPU)

### 4.1 Use cases
- 4.1.1 *As any non-AMD backend, when a kernel uses `Schedule.*`, then* it lowers and
  emits with the controls as **no-ops** (the default `LoweringTarget` seam) — the kernel
  compiles and runs correctly, just without the hint.
- 4.1.2 *When a kernel with `Schedule.*` is built for NVPTX/SPIR-V/CPU, then* a GPU-free
  emit/lower test confirms it produces valid output on each (parallel to
  `XpuAsyncCopyPortabilityTests`).
- 4.1.3 *(Optional, nearest-control)* *When NVPTX has a near-equivalent (e.g. a scheduling
  fence), then* the seam MAY map to it; otherwise no-op. Documented per backend.

---

## 5. Empirical f16 GEMM application

### 5.1 Use cases
- 5.1.1 *As the perf engineer, when I instrument `gemmF16` with `Schedule.*` hints
  (group-barrier WMMA/DS interleave in the K-loop, `s_setprio`, and a `pipelineOpt`
  probe), then* I measure each variant on HIP at n256–n2048 vs. the variant-B baseline,
  with rocprof, all `check_ok=true`.
- 5.1.2 *When a hinted variant beats variant B's 20.6 TFLOP/s @ n2048, then* it is shipped
  as the bench kernel and the rocprof evidence (what stall it cut) is recorded.
- 5.1.3 *When no hinted variant beats baseline, then* the result is recorded as an honest
  NO-GO with the rocprof evidence (e.g. iglp_opt was a WMMA no-op, group-barrier didn't
  change the issue pattern) — the portable surface still ships (valued for CDNA + future
  tuning), per §1.3.1.
