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

## 2. Automatic workgroup-size-aware register budgeting

### 2.1 Requirement
The compiler shall tell the device backend each `@Kernel`'s **actual launch workgroup
size**, so the backend budgets per-thread registers for the true (usually small)
occupancy instead of its pessimistic hardware-maximum default — eliminating spills that
exist only because the backend assumed the worst-case workgroup. The author's explicit
override (§3), if present, is respected verbatim.

### 2.2 Mechanism (AMDGPU first) — measured, corrected from the trial-codegen design
Empirical finding on gfx1151 (recorded in the plan): a register-heavy kernel left at the
default capped at **192 VGPR and spilled**; `"amdgpu-waves-per-eu"` had **no effect**;
setting **`"amdgpu-flat-work-group-size"`** to the real workgroup size raised the budget
to **256 VGPR and drove spill to 0**. Without the attribute the backend assumes the
1024-thread maximum (→ up to 32 waves/workgroup → low VGPR cap). So the mechanism is
*proactive*, not a spill-triggered retry:
1. Scan every module's `@Kernel` **launch sites** for the constant block dimensions and
   compute, per kernel, the **largest** workgroup size across its sites (product of the
   block dims).
2. At AMDGPU registration, set `"amdgpu-flat-work-group-size"="1,<maxThreads>"` on the
   kernel function (no explicit override present). NVPTX → `maxntid` (later); Vulkan →
   no-op.
3. **Soundness:** if *any* launch site of a kernel uses a non-constant block, the kernel
   is left at the backend default (we cannot bound its launch size) — never set a max a
   runtime launch could exceed.

This is superior to the trial-codegen spill-backoff: no extra codegen pass, it benefits
every kernel (not only spillers), and the value is the *correct* occupancy, not a guess.

### 2.3 Use cases
- 2.3.1 As a kernel author, when my register-heavy `@Kernel` is dispatched with a small
  block but spills under the backend's 1024-thread default budget, then the compiler
  removes the spill with no source change and no vendor knowledge on my part.
- 2.3.2 As a kernel author whose kernel does **not** spill, when I build, then the
  attribute is harmless (the budget simply matches the real workgroup) and correctness
  is unchanged.
- 2.3.3 As a maintainer, when a kernel is launched with a non-constant block, then the
  compiler conservatively leaves it at the default (sound) rather than pinning a bound.
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

## 4. Runtime autotuning: shipped guidance + fallback analysis (follow-on tier)

### 4.1 Requirement
For kernels whose best logistics depend on problem shape, config selection shall follow a
**three-tier lookup** so the common path costs nothing and only genuinely-new shapes pay
to learn:
1. **Runtime cache** — a config already chosen this run (or persisted from a prior run)
   for this (device, kernel, problem-shape) is used immediately.
2. **Shipped guidance** — a static tuning database compiled into the toolchain maps known
   (architecture, kernel, problem-shape) keys to their best config (authored offline from
   measurements — the same thing that gives Tensile/hipBLASLt their edge). A hit is used
   directly with **no on-device measurement** and seeds the runtime cache.
3. **Analysis phase** — on a miss, sweep a small list of default candidate configs, time
   each, pick the fastest, and write it to the runtime cache (so it is a hit thereafter).

This is "one better" than a pure first-launch sweep: most launches resolve against shipped
guidance and never measure; the sweep is the fallback for unknown shapes, and its results
accrete into the cache.

### 4.2 Architecture
- **TuningKey** = (arch, kernel name, problem-shape signature). The shape signature buckets
  the significant launch dimensions so near-identical shapes share a key.
- **TuningConfig** = the tunable launch logistics (v1: workgroup/block size; extensible to
  grid strategy and shared-memory split).
- The **selection** is pure decision logic (cache → guidance → sweep) with the timer
  **injected**, so it is validated GPU-free; the real launcher supplies an on-device timer.
- An `@Occupancy` override (§3) **clamps** the candidate set (e.g. swept block ≤ maxThreads)
  and the §2 compile-time pin bounds what the runtime may legally launch.

### 4.3 Use cases
- 4.3.1 As a user launching a shape covered by shipped guidance, when I run, then the best
  config is used with no measurement overhead.
- 4.3.2 As a user launching an unknown shape, when I run, then a short one-time analysis
  picks the best candidate and caches it; subsequent launches of that shape are hits.
- 4.3.3 As a user, when a config is already cached (this run or persisted), then neither
  guidance nor sweep runs — the cached winner is used.
- 4.3.4 As a user who pinned `@Occupancy` (§3), when autotuning runs, then the candidate
  set is clamped to my bound rather than ignoring it.
