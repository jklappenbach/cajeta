# Spec: Kernel occupancy autotuning (`kernel-occupancy-autotune`)

## 1. Definition

### 1.1 Purpose
GPU `@Kernel` performance depends on resource-scheduling parameters — occupancy
(waves/EU on AMD, blocks/SM on NVIDIA), per-thread register budget, and launch
bounds (workgroup size) — that affect **performance only, never correctness**. A
kernel is semantically identical whether it runs at 192 VGPR with register spilling
or 256 VGPR with none. These parameters are **logistics**, and the right value
differs per device, per problem shape, and per kernel.

Today Cajeta exposes no control over them and accepts whatever the backend register
allocator picks. Measured on gfx1151, the AMDGPU allocator capped the f16 WMMA GEMM
at 192 VGPR and **spilled 84 registers** rather than using the full 256 — choosing
an occupancy point that costs throughput on a compute-bound kernel. This spec defines
how Cajeta **detects and optimizes these logistics automatically**, with an optional
portable override for experts.

### 1.2 Scope
- **In:** (a) automatic, portable, compile-time elimination of register spilling by
  relaxing the occupancy target when the backend reports a spill; (b) one **portable**
  kernel annotation exposing the three logistics (occupancy target, register budget,
  launch bounds) as an optional override; (c) a runtime autotuning tier that sweeps a
  small config set per (device, problem shape) and caches the winner.
- **Out:** changing kernel *semantics*; vendor-specific source syntax; tuning
  parameters that are not pure logistics (algorithm/tile choices belong to the kernel
  author).

### 1.3 Non-goals
- Not a replacement for a hand-written kernel's algorithmic structure.
- The annotation must **never** leak vendor units (e.g. "waves per EU") into source.
- No regression to kernels that already do not spill (the auto path is a no-op there).

### 1.4 Principles
- **Automatic by default.** The compiler reads the backend's own resource report and
  fixes spilling itself; no annotation required.
- **Portable override only.** Manual control is one portable annotation that lowers
  per-backend and is a **no-op** where a backend has no equivalent.
- **Honest measurement.** Eliminating spill is guaranteed; a *throughput* win is not —
  every claim is measured on-device and recorded, never assumed.

## 2. Automatic compile-time spill backoff

### 2.1 Requirement
When the device backend compiles a `@Kernel` and its register allocator **spills**,
the compiler shall automatically relax that kernel's occupancy target (raising the
per-thread register budget) and recompile, keeping the spill-free result — unless the
author pinned an explicit override (§3), which is respected verbatim.

### 2.2 Mechanism (AMDGPU first)
1. After the device IR pipeline, perform a **trial codegen** to assembly and parse the
   per-kernel resource metadata (`.vgpr_spill_count`, `.vgpr_count`, `.name`) — the
   same metadata Cajeta already parses in its GPU-free probes.
2. For each kernel with `vgpr_spill_count > 0` and **no** explicit occupancy override,
   set `"amdgpu-waves-per-eu"="1"` (minimum occupancy → maximum VGPR budget) on that
   function.
3. The final codegen (object/hsaco) then uses the relaxed budget. Spill is eliminated
   when the kernel fits in the max budget; if it still spills at minimum occupancy the
   compiler has done its best and leaves it (logged).

### 2.3 Use cases
- 2.3.1 As a kernel author, when my register-heavy `@Kernel` spills under the
  allocator's default occupancy, then the compiler eliminates the spill with no source
  change and no vendor knowledge on my part.
- 2.3.2 As a kernel author whose kernel does **not** spill, when I build, then the auto
  path makes no change (identical output) and adds no measurable build cost.
- 2.3.3 As a compiler maintainer, when a kernel still spills at minimum occupancy, then
  I see a diagnostic recording the residual spill rather than a silent acceptance.
- 2.3.4 As a kernel author who pinned an override (§3), when I build, then the auto path
  does not touch my kernel — my choice wins.

## 3. Portable override annotation

### 3.1 Requirement
A single **portable** annotation shall let an author pin the logistics when they know
better than the auto path. It exposes all three logistics, uses no vendor units, and
lowers per-backend with a no-op where unsupported. It clamps/overrides the auto path.

### 3.2 Surface (portable; final names settled in the plan with the developer)
One annotation carrying up to three optional, portable parameters:
- an **occupancy target** (intent or abstract count — the register/parallelism trade),
- a **register budget** ceiling,
- **launch bounds** (max threads per group + minimum residency).

Lowering map (illustrative):
- AMDGPU → `amdgpu-waves-per-eu`, `amdgpu-flat-work-group-size` (and the register
  budget folded into the waves-per-eu target, since AMDGPU has no stable per-function
  VGPR-count attribute).
- NVPTX → `maxntid`, `minctasm`, `maxnreg`.
- Vulkan/SPIR-V → no-op.

### 3.3 Use cases
- 3.3.1 As an expert, when I annotate a kernel to prioritize register budget, then the
  AMDGPU build sets minimum occupancy and the NVPTX build sets the analogous launch
  bound — from one portable declaration.
- 3.3.2 As an author targeting a backend with no equivalent, when I use the annotation,
  then it compiles cleanly as a no-op (portable, never an error).
- 3.3.3 As an author, when I pin a value, then the automatic backoff (§2) respects it
  and does not override it.

## 4. Runtime autotuning (follow-on tier)

### 4.1 Requirement
For kernels whose best logistics depend on problem shape, the runtime shall sweep a
small set of occupancy/launch configurations on first launch per (device, kernel,
problem-shape), measure wall-clock, and cache the winner so subsequent launches use it.
This mirrors the autotuning database that gives vendor BLAS (Tensile/hipBLASLt) much of
its edge.

### 4.2 Use cases
- 4.2.1 As a user running a GEMM across sizes, when I launch a shape the first time,
  then the runtime picks the best config for that shape and reuses it thereafter.
- 4.2.2 As a user, when a cached config exists for my (device, kernel, shape), then no
  sweep occurs and the launch uses the cached winner immediately.
- 4.2.3 As a user, when I pin an override (§3), then the runtime treats it as a clamp on
  the sweep space rather than ignoring it.
