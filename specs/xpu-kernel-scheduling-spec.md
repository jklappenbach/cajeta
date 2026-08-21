# Spec: XPU live kernel scheduling & occupancy-maximizing deployment (`xpu-kernel-scheduling`)

> Status: **approved 2026-08-20**. The actionable *how* lives in
> [`agents/xpu-kernel-scheduling-plan.md`](../agents/xpu-kernel-scheduling-plan.md).
> §8's online feedback half is gated on [`cajeta-profiler`](cajeta-profiler-spec.md)
> Unit 7 (§12.6); §3's offline classification is not gated on anything unbuilt.

## 1. Definition

### 1.1 Purpose

Add a **session-level GPU scheduler** to the Cajeta XPU runtime that, given a
**live stream of kernel-launch and GPU-resource requests** arriving during a
process's lifetime, decides *when*, *where*, and *alongside what* each request
runs so as to **maximize whole-device occupancy and utilization** (SM/CU
compute and memory-bandwidth) while honoring each request's quality-of-service
class. It is the **inter-kernel / cross-request** layer, complementary to the
existing **intra-kernel** work:

- [`xpu-device-profile`](archive/xpu-device-profile-spec.md) — per-launch analytic
  launch-config picker + measured roofline.
- [`kernel-occupancy-autotune`](archive/kernel-occupancy-autotune-spec.md) — per-kernel
  register/occupancy budgeting + `@Occupancy`.
- [`xpu-kernel-scheduling-hints`](xpu-kernel-scheduling-hints-spec.md) —
  *instruction*-schedule hints inside one `@Kernel`.

Those tune a **single kernel in isolation**. This spec schedules a **stream of
many kernels from many logical operations** onto one (or a partitioned) device
so the device is never left half-idle by any single kernel's footprint.

The motivating fact (Orion, Salus, Volkov, roofline): a single kernel almost
never saturates *all* of a GPU's resources at once — a compute-bound GEMM leaves
memory bandwidth idle; a memory-bound reduction leaves the matrix cores idle; a
low-occupancy latency-bound kernel leaves SMs with spare warp slots. A live
scheduler that **co-runs complementary kernels** and **fills the gaps** recovers
that idle capacity.

### 1.2 Scope

- A `cajeta.xpu.sched` runtime component: a **request queue**, a **classifier**,
  a **scheduler core**, a **software-defined dispatch** layer, and a **feedback
  loop**, driving the existing per-backend launch path.
- Three co-existing sharing mechanisms, selected per situation:
  - **Temporal** — interleave/timeslice kernels on one queue (fast switch).
  - **Spatial** — concurrent kernels via multiple streams/queues, MPS
    fractions, or hardware partitions (MIG / CU masking).
  - **Preemptive** — priority preemption for deadline/latency classes.
- A **QoS-class model** covering three first-class workload profiles —
  **scientific/HPC**, **ML (training + inference)**, **graphics/real-time** —
  each with its own objective (throughput / latency-SLO / frame-deadline).
- Portable-by-design across the shipped backends (NVPTX, AMDGPU, Vulkan/SPIR-V,
  CPU fallback), degrading to the safe single-queue path where a mechanism is
  unavailable.

### 1.3 Non-goals

- **Cluster / multi-node scheduling.** This is a single-process, single-host
  (possibly multi-GPU) scheduler. Gandiva/AntMan/AlpaServe-style *cluster*
  placement is out of scope (referenced for mechanisms only).
- **A new hardware preemption mechanism.** We use what each backend exposes
  (CUDA/HIP streams + priorities, MPS, MIG, CU masks, cooperative
  yield-points); we do not modify drivers.
- **Replacing the intra-kernel tuners.** Occupancy/launch-config for a *single*
  kernel stays with `xpu-device-profile` / `kernel-occupancy-autotune`; this
  spec *consumes* their per-kernel footprints.
- **Guaranteeing hard real-time.** Graphics/control deadlines are *soft* here
  (best-effort priority + preemption), not a certified RT scheduler.

### 1.4 Principles

- **Occupancy is a means, not the end.** The target is *achieved device
  utilization* (SM-active %, mem-BW %, matrix-core %), not the theoretical
  occupancy number. Volkov: peak throughput can occur at *lower* occupancy.
  Co-scheduling is what turns spare occupancy into real work.
- **Complementarity over packing.** Two kernels co-run well when their
  bottlenecks differ (roofline classes differ). The scheduler pairs
  compute-bound with memory-bound, not compute-bound with compute-bound.
- **Interference-aware, at 10s-of-µs granularity.** Per-kernel decisions with a
  measured/estimated interference cost (Orion, iGniter), not coarse per-job
  exclusivity.
- **Predictable beats reactive.** Where execution time is deterministic (DNN
  inference — Clockwork), schedule proactively from a model instead of reacting
  to overruns.
- **The hardware block scheduler is opaque; give the runtime a software seam.**
  Concurrent kernels submitted naively are serialized or unfairly ordered by the
  GPU's built-in scheduler (Paella, Reef). Provide a software-defined
  submission/ordering layer.
- **Portable + correctness-preserving.** A scheduling decision never changes a
  kernel's results; on a backend lacking a mechanism it degrades to a documented
  safe path, never a hard error (same discipline as the shipped XPU specs).

## 2. The request model — the live stream

### 2.1 Requirement

Every unit of GPU work entering the runtime is a **`KernelRequest`** on a
per-session queue, carrying enough metadata to schedule it without re-deriving
it each time.

### 2.2 Mechanism

A `KernelRequest` records:

- **Identity**: kernel handle, grid/block dims, dynamic-smem bytes.
- **Footprint** (from the intra-kernel tuners): registers/thread, LDS/block,
  max blocks/SM, so the scheduler knows how much of one SM this kernel consumes.
- **Roofline class** (§3): `COMPUTE_BOUND` | `MEMORY_BOUND` | `LATENCY_BOUND`
  and an arithmetic-intensity estimate.
- **QoS class + objective**: `SCIENTIFIC` (throughput), `ML_TRAIN`
  (throughput/fairness), `ML_INFER` (latency SLO + batch), `GFX`
  (frame deadline), `CONTROL` (hard-ish latency). Plus priority and, where
  applicable, a deadline / SLO target.
- **Dependencies**: producer/consumer edges (events), so co-scheduling never
  violates ordering.

Requests arrive **continuously** (a session streams them); the scheduler holds a
bounded look-ahead window and (re)decides as new requests arrive and prior ones
complete.

### 2.3 Use cases

- A scientific solver streams thousands of independent stencil/GEMM kernels →
  scheduler packs complementary ones to keep both compute and BW busy.
- An inference server streams per-request kernels with latency SLOs →
  scheduler batches + co-locates without SLO violation.
- A renderer streams per-frame compute (culling, post-fx) with a 16.6 ms
  deadline → scheduler preempts best-effort work to protect the frame.

## 3. Kernel classification — roofline + occupancy footprint

### 3.1 Requirement

Each kernel is tagged with a **bottleneck class** and **SM footprint** so the
scheduler can reason about complementarity and packing.

### 3.2 Mechanism

- Reuse the **measured roofline** and `DeviceProfile` from `xpu-device-profile`:
  arithmetic intensity vs the device's compute/BW ridge point → `COMPUTE_BOUND`
  vs `MEMORY_BOUND`; a kernel that saturates neither at full occupancy is
  `LATENCY_BOUND` (Volkov regime).
- Footprint (regs, LDS, blocks/SM) comes from `kernel-occupancy-autotune`'s
  budgeting. Two kernels are **co-schedulable on the same SM set** if their
  combined per-SM footprint fits.
- Classification is **offline where possible** (compile-time estimate from the
  IR + `DeviceProfile`) and **corrected online** from the feedback loop (§8) —
  the first few executions calibrate the estimate (Clockwork-style profiling).

### 3.3 Use cases

- Pair `matmul-f16` (compute-bound, matrix-core heavy) with a memory-bound
  softmax/reduction so the reduction runs "for free" on idle BW.
- Detect a `LATENCY_BOUND` kernel and *raise* concurrency (more co-runners)
  rather than more blocks of the same kernel.

## 4. The scheduler core

### 4.1 Requirement

Given the look-ahead window of classified requests and the device's live state,
produce a **placement decision** per request: which sharing mechanism, which
queue/partition, at what priority, and whether to preempt.

### 4.2 Mechanism — a three-tier decision

1. **Complementarity co-scheduling (spatial, same device).** Greedily/ILP-lite
   pack requests whose roofline classes differ and whose combined footprint fits,
   dispatching them on **concurrent streams/queues** so the hardware runs them
   together. This is the primary occupancy lever (Orion). A packing score
   rewards differing bottlenecks and penalizes estimated interference (§5).
2. **Partitioning (spatial, hard isolation) when interference is high.** When
   co-runners would interfere badly (two compute-bound, or an SLO tenant next to
   a bursty best-effort one), fall back to **hard spatial partitions** — MIG
   instances (MISO), MPS compute fractions (iGniter), or CU/SM masks — sizing
   each partition to the tenant's footprint.
3. **Temporal timeslice + preemption for deadline/priority.** For
   latency/deadline classes, interleave via fast switching (Salus/PipeSwitch)
   and **preempt** lower-priority work when a deadline is at risk (Reef's
   reset-based instant preemption; cooperative yield-points where hardware
   preemption is coarse; the survey's real-time preemption taxonomy).

The core is a **policy over these three tiers**, chosen by the mix of QoS classes
currently in the window (see §7 for the per-class defaults).

### 4.3 Use cases

- Two scientific GEMMs of the same class → tier 2 (partition) rather than
  co-run (they'd fight for matrix cores).
- Inference batch + background training → tier 1 (co-run, complementary) with
  the inference batch on a high-priority stream, tier 3 preemption as a guard.

## 5. Interference model & admission

### 5.1 Requirement

Before committing a co-schedule, estimate the **slowdown** each co-runner
inflicts, and **admit** only combinations that keep SLO tenants within target.

### 5.2 Mechanism

- A **lightweight analytical interference model** (iGniter, Orion): predict each
  kernel's co-run latency from its solo profile + the co-runners' pressure on the
  shared bottleneck (matrix-core issue slots, L2/HBM bandwidth, LDS). MASK's
  finding — the *memory system* is the dominant interference channel — means the
  model weights shared-BW/TLB contention heavily.
- **Admission control**: an SLO/deadline request is admitted to a co-schedule
  only if the model predicts it still meets target; otherwise escalate to tier 2
  (partition) or tier 3 (preempt/serialize). Model error is corrected online
  (§8).

### 5.3 Use cases

- Reject co-locating two BW-bound kernels that would each halve throughput;
  serialize or partition instead.
- Admit a compute-bound + memory-bound pair the model predicts at <5% mutual
  slowdown.

## 6. Software-defined dispatch (the interception seam)

### 6.1 Requirement

The runtime must **control submission order and concurrency**, not hand kernels
to the opaque hardware block scheduler and hope.

### 6.2 Mechanism

- All `@Kernel` launches route through the scheduler's **dispatch layer** (the
  existing per-backend launch path, extended). It owns a pool of
  streams/queues with priorities and decides which request goes on which,
  when — the Paella/Reef "software-defined GPU scheduling" idea, adapted to
  Cajeta's `LoweringTarget` seam.
- Per backend:
  - **NVPTX/CUDA**: multiple streams + stream priorities; optional **MPS**
    fractions; **MIG** partitions; CUDA Graphs for low-overhead re-submission.
  - **AMDGPU/HIP**: HIP streams + priorities; CU masking for spatial partitions.
  - **Vulkan/SPIR-V**: multiple compute queues + async-compute; per-queue
    priorities (graphics use case).
  - **CPU fallback**: the thread-pool executor already in the runtime; the
    scheduler degrades to ordinary task scheduling.
- **Portability rule**: a mechanism absent on a backend (e.g. MIG on consumer
  GPUs) degrades to the next tier (MPS → streams → single queue), never an
  error.

### 6.3 Use cases

- Force a high-priority inference kernel ahead of a queued training kernel the
  hardware would otherwise have ordered arbitrarily.
- Batch many tiny kernels into a CUDA Graph to cut launch overhead when the
  stream is launch-bound.

## 7. Per-workload-class policies

### 7.1 Requirement

Each QoS class gets a **default policy** (objective + preferred tier), because
scientific, ML, and graphics workloads optimize different things.

### 7.2 Mechanism

| Class | Objective | Default tier | Key levers |
|---|---|---|---|
| **Scientific / HPC** | max throughput, high occupancy | tier 1 co-run; tier 2 when same-class | roofline pairing; large-grid persistent kernels; BW/compute complementarity |
| **ML training** | throughput + fairness across jobs | tier 1 co-run + tier 3 timeslice | iteration-granularity switching (Salus/Gandiva); harvest idle capacity for opportunistic jobs (AntMan/TGS) |
| **ML inference** | latency SLO at max throughput | tier 1 co-run under admission + adaptive batching | deterministic-time scheduling (Clockwork); batch (Clipper/Nexus); interference-aware provisioning (iGniter/Orion); statistical multiplexing of bursts (AlpaServe) |
| **Graphics / real-time** | frame deadline, smoothness | tier 3 preemptive priority + async-compute co-run | frame-deadline priority; async-compute queues fill shadow/geometry gaps; preempt best-effort compute before present (survey's RT taxonomy; Reef-style fast preemption) |
| **Control / latency-critical** | bounded latency | tier 3 preempt + reserved partition | reserved MIG/CU slice; highest preemption priority |

Mixed sessions (e.g. an ML app that also renders a UI, or a scientific viz tool)
compose these: the scheduler runs the highest-criticality class's policy for
admission/preemption and fills the remainder with lower-class best-effort work.

### 7.3 Use cases

- A game streams graphics + a background ML NPC model: GFX frames preempt; the
  ML model co-runs in async-compute gaps and yields before present.
- An HPC session interleaves a compute-bound solver with memory-bound I/O
  staging kernels for near-100% combined utilization.

## 8. Feedback & metrics

### 8.1 Requirement

Close the loop: measure what actually happened and adapt the classifier and
interference model.

### 8.2 Mechanism

- **Signals**: per-kernel achieved vs predicted latency, taken from
  [`cajeta-profiler`](cajeta-profiler-spec.md)'s dispatch-record seam (§5.6) — the
  scheduler registers a **live record sink** and receives device start/end per
  launch, in-process, already in the host clock domain and marked with the tier
  that produced it. An earlier draft of this section sourced these from "the
  existing rocprof/CUPTI hooks used by `xpu-device-profile`"; that tier is opt-in,
  in-memory and built to calibrate a launch-config picker, and it emits no
  per-launch stream. The profiler seam does, and was made dual-consumer for this
  (that spec's §14.9). Secondary: device counters where available (SM/CU active %,
  mem-BW %, matrix-core busy); SLO attainment; deadline misses.
- **Sink discipline**: the seam drops rather than blocks (`cajeta-profiler` §5.6.4),
  so the feedback loop must treat its record stream as **lossy by design** — a
  sampled correction, never a ledger. A policy that requires having seen every
  launch is a policy this loop cannot support.
- **Adaptation**: online correction of the roofline class and interference
  weights (Clockwork/iGniter online profiling); demote a co-schedule that
  under-delivers; promote pairs that measured well; feed the device model.
- **Primary KPIs**: (1) achieved device utilization (SM% × mem-BW% envelope),
  (2) SLO/deadline attainment per class, (3) tail latency (p99), (4) fairness
  across same-class tenants, (5) throughput/Watt.

### 8.3 Use cases

- The loop discovers a mispredicted pair on a specific device, records the
  correction in the `DeviceProfile`, and stops co-scheduling it.

## 9. Best-of-breed survey

Surveyed systems (full corpus + markers in `research/xpu-scheduling/papers/`)
and the idea taken from each:

| Dimension | Source(s) | What we take |
|---|---|---|
| Fine-grained interception, per-op | Orion, Paella | intercept every launch; schedule at kernel granularity through a software seam |
| Complementarity co-run | Orion, roofline (Williams), Volkov | pair differing roofline classes; occupancy≠utilization |
| Fast temporal switch | Salus, PipeSwitch | iteration-granularity job switching for time-sharing |
| Instant preemption | REEF | reset-based preemption + kernel padding for deadline classes |
| Deterministic scheduling | Clockwork | proactive schedule from a predictable-time model (inference) |
| Adaptive batching | Clipper, Nexus | batch to trade latency for throughput under SLO |
| Interference model / provisioning | iGniter, Orion, MASK | analytical slowdown model; memory system is the dominant channel |
| Hardware spatial partition | MISO (MIG), iGniter (MPS), choi (spatio-temporal) | hard isolation tier when co-run interferes |
| Capacity harvesting | AntMan, TGS, Gandiva | fill idle GPU with opportunistic jobs, protect SLO jobs |
| Statistical multiplexing | AlpaServe | multiplex bursty request streams to cut tail latency |
| Real-time / deadline taxonomy | RT-accelerator survey, REEF | preemption + priority + partitioning for time-critical (gfx/control) |
| Launch-config / occupancy footprint | xpu-device-profile, kernel-occupancy-autotune (this repo) | per-kernel footprint + roofline inputs to the scheduler |

## 10. Kernel gap catalog — what the library must add

The scheduler classifies and co-schedules kernels; it can only schedule kernels
that exist. Today the library has **7** (`arithF32`/map, `reduceSumF32`,
`gatherF32`, `matmulF32`/GEMM, `bitonicStepF32`, `fftStageF32`, `philoxUniformF32`/
RNG), covering 5 of the Berkeley "13 dwarfs" computational motifs. The workload
profiles above (scientific, ML, graphics/robotics) repeatedly need the same
missing primitives; this catalog is the scheduler-facing view of that gap. The
prioritized build order is tracked in the (local) `xpu-kernel-library` plan.

| Primitive | Roofline | Status | Needed by |
|---|---|---|---|
| map / elementwise | memory | **have** (`arithF32`) | all |
| reduce | memory | **have** (`reduceSumF32`) | all |
| gather | memory | **have** (`gatherF32`) | all |
| dense GEMM | compute | **have** (`matmulF32`) | ML, scientific, gfx |
| FFT stage | mixed | **have** (`fftStageF32`) | scientific, signal |
| sort step | memory | **have** (`bitonicStepF32`) | gfx, planning |
| RNG | compute | **have** (`philoxUniformF32`) | Monte-Carlo, ML |
| **scan / prefix-sum** | memory | **missing** | gfx culling/compaction, LLM batching, robotics hashing — *foundational* |
| **scatter / scatter-add** | memory | **missing** | gfx binning, MoE routing, voxel hashing, histogram |
| **stream compaction / filter** | memory | **missing** | gfx cluster compaction, planning pruning |
| transpose, histogram, segmented reduce/scan, argmax/minmax | memory | **missing** | pervasive |
| **stencil / convolution** | compute/memory | **missing** | scientific PDEs, gfx blur/AO, ML conv |
| **SpMV / SpMM (sparse LA)** | memory (irregular) | **missing** | scientific solvers, SLAM bundle adjustment |
| **radix sort (full)** | memory | **missing** | gfx transparency/particles, planning |
| **attention (flash + paged)** | prefill compute / decode memory | **missing** | LLM serving, robot VLA |
| **softmax / LayerNorm / RMSNorm** | memory | **missing** | ML, LLM |
| **fused activation epilogues** | memory | **missing** | ML, LLM |
| **sampling / top-k** | memory | **missing** | LLM decode |
| **collectives (all-reduce, all-to-all)** | interconnect | **missing** | tensor/expert-parallel LLM, distributed BA |

**Critical path:** `scan` + `scatter` gate nearly everything downstream
(compaction, sort, histogram, routing, sparse builds, GPU-driven culling,
continuous batching) — they are the highest-leverage first additions.

## 11. Integration with the Cajeta XPU runtime

- Lives in `cajeta.xpu.sched`, driven by the existing `@Kernel` launch path and
  `LoweringTarget` per-backend seam.
- Consumes `DeviceProfile` (roofline, machine model) and per-kernel occupancy
  budgets already produced by the shipped XPU work — those are the *offline*
  classification inputs (§3.2).
- Consumes `cajeta-profiler`'s dispatch records through a registered live sink for
  the *online* correction (§8.2). This is the one input that is not yet built: it
  arrives with that spec's Unit 7, which ships the seam and the trace-writer sink;
  the scheduler's own sink ships here.
- Surface: a portable `@Kernel` launch site gains optional QoS/priority/deadline
  metadata (defaulting to `SCIENTIFIC`/throughput), and a session-level
  `Scheduler` handle for policy selection. Exact annotation names settle in the
  plan with the developer (mirrors `@Occupancy`'s naming process).
- Correctness/portability discipline identical to the shipped XPU specs:
  results-invariant, on-device-measured wins, safe degradation per backend.

## 12. Risks / dependencies

1. **Hardware-scheduler opacity** — concurrent streams don't guarantee true
   co-execution on all GPUs; MPS/MIG/CU-mask availability varies. Mitigation:
   tiered degradation (§6), measure-don't-assume.
2. **Preemption granularity** — commodity GPUs preempt coarsely; REEF-style
   reset and cooperative yield-points needed for µs deadlines. Graphics async
   preemption differs by vendor.
3. **Interference-model error** — mispredictions violate SLOs; the online
   feedback loop (§8) and admission conservatism bound the damage.
4. **Profiling-counter availability** — SM/BW counters differ across
   backends/drivers; the loop must degrade to latency-only signals. Those remain
   available, because per-launch latency comes from the profiler seam rather than
   from counters — and `cajeta-profiler` §1.4.1 excludes hardware counters
   outright, so the counter tier is this spec's own problem to source, not
   something that seam will ever supply.
5. **Scheduling overhead** — the scheduler itself must stay off the critical
   path (10s of µs budget); use CUDA Graphs / batched submission.
6. **Dependency on an unapproved spec** — the online half of §8 cannot be built
   until `cajeta-profiler` Unit 7 exists. The offline half (§3.2 classification
   from `DeviceProfile` + occupancy budgets) has no such dependency and can land
   first; sequence the plan so the scheduler is useful before the feedback loop
   closes, rather than blocked behind it.

## 14. Resolved decisions

Closed with the developer 2026-08-21.

- **14.1 v1 ships all five QoS classes, with two of them unverified on available
  hardware.** `SCIENTIFIC`, `ML_TRAIN`, `ML_INFER`, `GFX` and `CONTROL` all ship,
  so §7.2's policy table lands whole and mixed-session composition is exercised
  across the full range. **`GFX` and `CONTROL` are the deadline classes and their
  guarantees cannot be verified here.** Unit 7.3.a requires a deadline-class
  request to meet its deadline with best-effort work running concurrently, and
  §12.2 warns that commodity GPUs preempt coarsely enough to need REEF-style
  reset and cooperative yield-points for µs deadlines. gfx1151 is an APU with
  limited preemption granularity and the RTX 4090 sits behind CI. Those two
  classes therefore ship implemented and **explicitly marked unverified**, on the
  same terms as §14.2 — never as a silent gap. A deadline guarantee that has
  never been measured must not read like one that has.
- **14.2 The MIG path is built and marked untested.** Neither reference device
  exposes MIG, so tier 2's top rung ships without ever having run against real
  hardware. It sits behind the same degradation ladder as everything else
  (§6.2), so consumer GPUs take MPS or streams regardless and cannot silently
  select it. The plan and the trace both record that it is unverified. Rejected:
  deferring MIG entirely, which would leave §4.2.2's partitioning story
  incomplete on paper; and gating it behind a flag, which adds a switch whose
  only purpose is to quarantine untested code.
- **14.3 Implementation starts after `cajeta-profiler` Unit 7.** §3's offline
  classification is unblocked today — it reuses the shipped `DeviceProfile`
  roofline and occupancy budgets — but starting now would put two half-finished
  plans in one working copy and a context switch in every session. §8's online
  half needs the dual-consumer record seam regardless (§12.6). Rejected: running
  the offline half in parallel, and running it in a sibling clone, which the
  per-clone focus state would have supported but which still splits attention
  across two plans.

## 13. References

Full PDFs + markers in [`research/xpu-scheduling/papers/`](../research/xpu-scheduling/papers/):
Salus, REEF, Orion, Paella, TGS, AntMan, Gandiva, PipeSwitch, Clockwork,
Clipper, Nexus, AlpaServe, iGniter, MISO, choi (spatio-temporal serving), MASK,
Volkov (occupancy), Williams (roofline), and the real-time accelerator
scheduling survey. Vendor references: CUDA MPS & MIG user guides, CUDA
streams/priorities & CUDA Graphs, HIP stream API + CU masking, Vulkan
async-compute queues. Sibling specs: `xpu-device-profile`,
`kernel-occupancy-autotune`, `xpu-kernel-scheduling-hints`,
[`cajeta-profiler`](cajeta-profiler-spec.md) (§5.6 record seam, §8 feedback source).
