# GPU GEMM Occupancy & WMMA Scheduling — Spec

## 1. Definition

### 1.1 Purpose
Close (as far as the hardware and the `@Kernel` source model allow) the remaining f16
WMMA GEMM performance gap to torch/rocBLAS on **gfx1151** (RDNA3.5, Strix Halo) by
attacking the **two levers U4 identified as where the gap actually lives**: GPU
**occupancy / VGPR pressure**, and **WMMA instruction scheduling**. The effort is
**kernel-first** — exhaust what `@Kernel` source tuning can reach, find the wall, then add
only the **compiler/language features** the tuning proves it is blocked on. Outcome is
**empirical and honest** (per U4): measure each lever's TFLOP/s delta, ship the wins,
record the residual + the next-identified bottleneck. There is **no hard parity gate**.

### 1.2 Problem
The Cajeta f16 WMMA GEMM (`samples/profile` `GpuMatMulF16Bench`, the
`v_wmma_f32_16x16x16_f16` path rocBLAS/PyTorch also use) measures **17.5 TFLOP/s at
n=2048 on gfx1151 HIP** — roughly 60% of torch's CK-tuned f16. The `xpu-pipelined-gemm-
primitives` plan (U1–U4) proved that the *memory-staging* primitives do **not** close this
gap on this hardware:
- **AsyncCopy** has no native acceleration on gfx1151 — RDNA3.5 lacks `VMemToLDSLoad`, so
  the direct global→LDS load is unavailable ([[reference_gfx1151_no_vmem_to_lds]]).
- **Swizzled<T,S>** (conflict-free LDS) is **net-negative** here (U4 measured
  1.16–1.44× *slower*): the f16 GEMM is **not LDS-bank-conflict-bound**, so the swizzle's
  extra per-fragment address arithmetic costs more than any conflict savings.

What remains — what CK/rocBLAS actually tune — is **occupancy and scheduling**:
- **VGPR pressure → occupancy.** The current kernel uses **4 f32 WMMA accumulators per
  wave** (a 2×2 grid of 16×16 tiles). Each f32 accumulator is a `<8 x i32>` fragment, so
  the accumulators alone are VGPR-heavy; with the A/B fragments + addressing, VGPRs/wave
  caps how many waves run per SIMD, leaving WMMA / memory latency unhidden. gfx1151 has a
  fixed VGPR file per SIMD; theoretical occupancy = floor(file / vgprs-per-wave).
- **WMMA scheduling.** CK interleaves `mma` with the next panel's loads and steers the
  instruction scheduler (`s_setprio`, `sched_group_barrier`, `iglp_opt`) so the matrix
  cores stay fed. Cajeta emits a straightforward order with no scheduling hints.

### 1.3 Scope
1. **Occupancy / VGPR pressure** (§2): measure the baseline VGPR/wave + theoretical
   occupancy, then reduce it via source tuning (fewer accumulators/wave, tile & wave-count
   sweeps) and measure the occupancy↑ / TFLOP/s effect; identify the VGPR wall source can't
   cross.
2. **WMMA instruction scheduling** (§3): source-level interleave / pipeline-depth
   experiments, then identify the scheduling control source can't express.
3. **Compiler / language features** (§4) — **only the ones §2/§3 prove are blocking**, and
   each specced **portable-by-design**: an architecture-neutral `@Kernel` surface (an
   occupancy / register-budget hint, a scheduling-hint abstraction) with a **per-backend
   lowering seam** (AMD implemented now; NVPTX / SPIR-V seam in place, stubbed / no-op until
   their plan), plus any internal accumulator-regalloc improvement.

**f16 only.** Levers found here are expected to transfer to bf16/int8, but this effort
measures and tunes one dtype.

### 1.3.1 Design principle — maximize the portable union
The strategy accepts that some effort is irreducibly vendor-specific (gfx1151's VGPR file,
the AMD scheduling instructions, the tuned constants). The discipline is to keep **as much
as possible in the portable union**: the `@Kernel`-facing surface of every §4 feature is
**architecture-neutral**, and only the **lowering** (the per-backend `LoweringTarget` seam)
and the **tuned constants** are vendor-specific. A kernel author writes a portable occupancy
or scheduling hint; the AMD backend honors it via AMD mechanisms today, and NVPTX/SPIR-V
honor it (or no-op it) through the same seam later — never a fork in the source. This mirrors
the existing XPU seam model and the "one unified model" / "promote to general-purpose"
conventions.

### 1.4 Constraints
- **gfx1151 / RDNA3.5** is the target and the only on-device hardware here.
- **Additive** — must not regress the existing GEMM correctness (`check_ok=true` at every
  swept size) or any other bench; the f16 bench's best-performing version is what ships.
- **Verification tiering** (see §5): VGPR count + theoretical occupancy are **GPU-free**
  (hsaco metadata / ISA), so occupancy is measurable without a device; TFLOP/s is
  **on-device HIP** (available here). The rocprof occupancy/stall *counters* are
  **deferred** (the D1 perf item, [[reference_swizzle_conflict_replay_rocprof_deferred]]).
- **Kernel-first** is a hard ordering: a compiler/language feature (§4) is only justified
  once a §2/§3 source experiment shows the tuning is blocked on it — no speculative knobs.

### 1.5 Non-goals
- **A hard parity gate** (≥ torch). The success criterion is measure-ship-record (§1.1).
- **bf16 / int8** WMMA occupancy tuning — a transfer follow-up, not this effort.
- **CDNA / MFMA**, and **fp64** GEMM (separately crowned, [[project_gpu_matmul_tiling_crown]]).
- **Re-touching the WMMA *correctness* lowering** (`coopMatrixLoad/Store/MulAdd`) — those
  are correct; this is about occupancy/scheduling, not fragment layout.
- **Memory-staging primitives** (AsyncCopy / Swizzled) — done; U4 proved them net-neutral
  to net-negative on this GEMM/hardware.
- **rocprof-counter-based** verification now (deferred D1); occupancy here is the
  metadata-derived *theoretical* figure + on-device timing.
- **Native NVPTX / SPIR-V *lowering* of the §4 portable hints.** The portable `@Kernel`
  surface and the cross-backend seam ARE in scope (so the hints compile everywhere); the
  *AMD* lowering is the one implemented + measured here. A full native NVPTX/SPIR-V mapping
  (beyond a correct stub/no-op) is a transfer follow-up, not this effort.

---

## 2. Occupancy / VGPR pressure

The first and likely dominant lever: fewer VGPRs per wave → more waves per SIMD → latency
hidden. Measured GPU-free (VGPR count, theoretical occupancy) + on-device (TFLOP/s).

### 2.1 Use cases
- 2.1.1 As a developer, I can read the **baseline** VGPRs/wave and **theoretical occupancy**
  (waves/SIMD) of the current `gemmF16` kernel from its compiled hsaco metadata / ISA — with
  no GPU — so the occupancy starting point is a concrete number, not a guess.
- 2.1.2 As a developer, when I reduce the per-wave accumulator count (4 → 2 → 1 f32
  accumulators, compensating with more waves / smaller per-wave output tiles), the kernel
  stays `check_ok=true`, its VGPRs/wave drop, its theoretical occupancy rises, and I have a
  measured TFLOP/s for each configuration at the swept sizes (n256…n2048) on HIP.
- 2.1.3 As a developer, I can sweep the **tile shape × wave-count** trade-off (the
  occupancy ↔ data-reuse tension — fewer accumulators means less register reuse but more
  waves) and identify the configuration with the best measured TFLOP/s, shipping that as the
  bench kernel if it beats the 17.5 TFLOP/s baseline.
- 2.1.4 As a developer, I can identify the **VGPR wall** — the point where `@Kernel` source
  changes can no longer reduce VGPRs/wave (e.g. the accumulator fragments the compiler
  insists on materializing, or spills the compiler introduces) — naming the specific
  compiler lever (§4) that would cross it.

---

## 3. WMMA instruction scheduling

Keep the matrix cores fed: overlap `mma` with the next panel's loads, and (where source
can't) steer the instruction scheduler the way CK does.

### 3.1 Use cases
- 3.1.1 As a developer, when I reorder the `@Kernel` source so the WMMA `mma` calls
  interleave with the next K-panel's staging loads (rather than a strict stage-then-compute
  order), the kernel stays correct and I have a measured TFLOP/s delta — quantifying how much
  source-level interleaving alone buys.
- 3.1.2 As a developer, I can vary the **software-pipeline depth** (beyond the current
  double-buffer — e.g. a deeper prefetch of A/B panels) in source and measure whether more
  in-flight staging hides more WMMA/memory latency, or whether it just costs VGPRs (the
  occupancy tension from §2).
- 3.1.3 As a developer, I can identify the **scheduling control** `@Kernel` source cannot
  express — instruction-group priority / ordering hints (`s_setprio`, `sched_group_barrier`,
  `iglp_opt`) — naming the compiler intrinsic (§4) that would expose it, with the measured
  ceiling source-only scheduling reached.

---

## 4. Portable occupancy & scheduling controls (driven by §2–§3 walls)

**Conditional** — each item here is in scope **only if** a §2 or §3 source experiment proves
the kernel is blocked on it. The kernel-first phase decides which (if any) are built. Every
feature is **portable-by-design** (§1.3.1): an architecture-neutral `@Kernel` surface + a
per-backend `LoweringTarget` seam, AMD implemented first, NVPTX/SPIR-V seam present (stubbed
/ no-op) so the portable surface compiles everywhere from day one.

### 4.1 Use cases
- 4.1.1 As a kernel author, I declare a **portable occupancy / register-budget hint** on any
  `@Kernel` (e.g. a minimum-waves or register-ceiling attribute) and it compiles on **every**
  backend through one seam — the **AMD** lowering honors it via `amdgpu-waves-per-eu` /
  `.amdhsa_next_free_vgpr`, raising occupancy to the tuned optimum (verified GPU-free in the
  hsaco metadata). The same attribute maps to NVPTX `__launch_bounds__`/`maxnreg` and a
  SPIR-V equivalent (or a documented no-op) through the seam — never a source fork.
- 4.1.2 As a kernel author, I express a **portable scheduling hint** (an
  instruction-group / priority-barrier abstraction) so my source-expressed `mma`/load
  interleave is preserved and steered. The **AMD** seam lowers it to `sched_group_barrier` /
  `s_setprio` / `iglp_opt` (verified by the hint in the emitted ISA + a measured on-device
  delta); other backends lower it to their nearest control or a no-op through the same seam.
- 4.1.3 As the compiler, when a kernel's accumulator fragments dominate VGPR pressure, the
  AMD path allocates them without unnecessary spills / copies (the §2.1.4 wall), verified by
  a reduced VGPR count in the metadata and no `scratch`/spill in the ISA. (The improvement is
  in the AMD lowering; the principle — don't spill accumulators — applies to every backend.)

---

## 5. Measurement & verification

- 5.1 **GPU-free (no device):** VGPRs/wave and theoretical occupancy from the compiled
  kernel's hsaco metadata / ISA resource directives; presence/absence of scheduling hints
  and spills in the emitted ISA. Every §4 claim has a GPU-free emit/metadata assertion.
- 5.2 **On-device (gfx1151 HIP, available):** TFLOP/s at the swept sizes (n256…n2048) with
  `check_ok=true`, A/B'd against the 17.5 TFLOP/s baseline — the same harness U4 used.
- 5.3 **Deferred (D1):** rocprof occupancy / wavefront-stall counters that would *confirm*
  the theoretical occupancy translates to wall-clock latency hiding — the on-device profiler
  item, run during a profiling pass ([[reference_swizzle_conflict_replay_rocprof_deferred]]).
- 5.4 **Honest residual:** each lever's measured delta is recorded; the shipped kernel is the
  best-measured config; the remaining gap to torch + the next-identified bottleneck are
  written down (per U4's framing).

## 6. Acceptance / fidelity
- 6.1 The baseline VGPR/wave + theoretical occupancy are measured (GPU-free) and recorded.
- 6.2 Each occupancy (§2) and scheduling (§3) source lever has a measured TFLOP/s delta vs
  the 17.5 baseline at the swept sizes, `check_ok=true` throughout.
- 6.3 The best-measured configuration ships as the bench kernel (only if it beats baseline);
  no regression to correctness or other benches.
- 6.4 Any §4 feature built is justified by a named §2/§3 wall, exposes a **portable,
  architecture-neutral `@Kernel` surface** that compiles on every backend (AMD lowering
  implemented; NVPTX/SPIR-V seam present, stubbed/no-op), has a GPU-free emit/metadata test,
  and a measured on-device delta on AMD.
- 6.5 The residual to torch + the next-identified bottleneck are recorded honestly; levers
  that did not help are recorded as such (not silently dropped).
