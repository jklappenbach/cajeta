# Spec: Robotics kernels & scheduling on limited hardware (`robotics-kernel-scheduling`)

## 1. Definition

### 1.1 Purpose

Characterize the **kernels an autonomous robot runs** and the **scheduling they
demand on limited, heterogeneous, power-constrained hardware**, as a first-class
workload for the XPU orchestrator
([`xpu-kernel-scheduling`](xpu-kernel-scheduling-spec.md)). Robotics is the hardest
scheduling regime: a **fixed, small compute budget** (an edge SoC), **multiple
concurrent pipelines at different rates**, **hard real-time deadlines** for
safety-critical control alongside best-effort perception, and an **energy/power
budget** that latency and throughput must respect.

### 1.2 Scope

- The robot compute pipeline — **perception → state estimation/SLAM → planning →
  control**, plus **learned policies** (VLA / diffusion) — and the kernels each
  stage employs.
- The **multi-rate, mixed-criticality** structure (sensor/control periods) and its
  real-time scheduling implications.
- Scheduling on a **heterogeneous SoC** (GPU + DLA/NPU + CPU real-time cores +
  vision accelerators) with **unified-memory contention**, **oversubscription**
  (more models than the GPU can run at once), and **energy** as a first-class
  objective.
- The mapping onto the orchestrator's `CONTROL` / `SCIENTIFIC` / `ML_INFER`
  classes and a robotics composition policy.

### 1.3 Non-goals

- Not robot algorithms themselves (SLAM/planning theory); this specs the kernel +
  scheduling substrate they run on.
- Not multi-robot/cloud offload orchestration (referenced for the offload
  decision only).
- Not a safety-certification methodology; we provide predictability, not a
  certificate.

### 1.4 Principles

- **Fixed budget, many tenants → oversubscription is the norm.** Unlike the
  datacenter, you cannot add GPUs; the scheduler must time/space-share one small
  accelerator across perception, mapping, planning, control (DARIS-style
  oversubscribed real-time scheduling).
- **Mixed criticality: hard deadlines beat throughput.** A missed control/safety
  deadline is a failure, not a slowdown. Reserve capacity and preempt for the
  hard-RT class; fill the rest best-effort.
- **Multi-rate: schedule periods, not just kernels.** IMU (~1 kHz), control
  (~100 Hz–1 kHz), camera (30–60 Hz), LiDAR (10–20 Hz), planning (~1–10 Hz) run at
  different rates; the scheduler is periodic/rate-based, not purely reactive.
- **Heterogeneous engines: place each kernel on its best engine.** Offload DNN
  inference to the DLA/NPU, geometry/linear-algebra to the GPU, tight control to
  real-time CPU cores; partition by engine and manage the shared memory
  bottleneck (the dominant interference channel on unified-memory SoCs).
- **Energy is an objective, not a constraint to ignore.** Power/payload budgets
  bound sustained compute; the scheduler trades detail/rate/precision (e.g.
  quantized inference) to hold deadlines within the power envelope.

### 1.5 Kernel status (current vs planned)

The robot stack draws on a few built kernels but needs a superset of the missing
primitives.

- **Built (reusable here):** `matmulF32` (dense solves / control), `reduceSumF32`,
  `gatherF32`, `philoxUniformF32` (sampling planners), `fftStageF32`.
- **Must build (see the local `xpu-kernel-library` backlog + master-spec gap
  catalog):** `SpMV`/sparse-solve (bundle adjustment, pose-graph), `stencil`
  (optical flow, stereo, TSDF), `scatter`/hash (voxel hashing, histogram),
  nearest-neighbor `gather`+reduce (ICP), and the ML kernels (`attention`, `conv`,
  norms) for perception/VLA. `scan` + `scatter` underlie the spatial structures.

## 2. The kernel taxonomy by stage

### 2.1 Requirement

Enumerate the kernels per pipeline stage with roofline class and criticality, so
the orchestrator can place and co-schedule them.

### 2.2 Mechanism

| Stage | Representative kernels | Primitive / dwarf | Roofline | Criticality |
|---|---|---|---|---|
| **Sensor preprocess** | undistort/resize, debayer, IMU integrate | map, gather | memory-bound | soft, high-rate |
| **Perception (DNN)** | conv, attention, GEMM, norm, activation | Dense LA + Attention | mixed (compute-bound convs, memory-bound norms) | soft (deadline per frame) |
| **Feature / flow / stereo** | feature extract, optical flow, stereo match | stencil, gather, histogram, dynamic programming | memory-bound | soft |
| **State estimation / SLAM** | bundle adjustment, pose-graph opt, ICP, EKF | **Sparse LA**, N-body/NN-gather, dense LA | memory-bound (sparse), compute (solve) | soft–firm |
| **Mapping** | TSDF/voxel fusion, voxel hashing, ray cast | **Structured grid**, scatter/hash | memory-bound | soft |
| **Planning** | sampling (RRT), trajectory opt, collision check, graph search | RNG + geometry, dense/sparse LA, **graph traversal**, spatial hash | mixed | firm |
| **Control** | MPC/QP solve, Kalman update, PID | dense LA (small) | latency-bound (tiny) | **hard, high-rate** |
| **Learned policy** | VLA transformer / diffusion denoise | Attention + GEMM (LLM kernels), iterative conv/GEMM | mixed | firm, at control/actuation rate |

The heavy compute-bound kernels (convs, GEMM, dense solves) again pair with many
memory-bound ones (preprocess, norms, sparse SpMV, fusion) — the same
complementarity lever, but now inside a **fixed budget with hard deadlines**.

### 2.3 Use cases

- Perception conv co-runs with a memory-bound TSDF fusion; the hard-RT control QP
  gets a reserved slice and preempts both when its period fires.

## 3. Multi-rate, mixed-criticality scheduling

### 3.1 Requirement

Serve several periodic pipelines at their rates while guaranteeing the hard-RT
class never misses a deadline.

### 3.2 Mechanism

- **Rate/period model**: each pipeline is a periodic task with a period + deadline
  + criticality. The scheduler admits a schedulable set (utilization-bounded) and
  rejects/degrades the rest.
- **Criticality tiers**: hard-RT (control/safety) → reserved partition + highest
  preemption priority (orchestrator tier-3 + tier-2 reservation); firm (planning,
  policy) → deadline-ordered; soft (perception/mapping) → best-effort, fills idle
  capacity (tier-1 co-run).
- **Graceful degradation under overload**: drop to lower rate / lower detail /
  quantized model rather than miss a hard deadline (energy + deadline governor),
  mirroring the gfx streaming spec's coarser-band fallback.

### 3.3 Use cases

- Under a compute spike, perception drops from 30→15 Hz and switches to a
  quantized model; control stays at 1 kHz, uninterrupted.

## 4. Heterogeneous placement + memory contention

### 4.1 Requirement

Place each kernel on the SoC engine that runs it best, and manage the shared
(unified) memory that all engines contend for.

### 4.2 Mechanism

- **Engine affinity**: DNN inference → DLA/NPU; geometry/linear-algebra/parallel
  primitives → GPU; tight control loops → real-time CPU cores; image ops →
  vision accelerator. The orchestrator's partition tier extends to *engine
  assignment*, not just GPU partitions.
- **Memory-contention-aware co-scheduling**: on unified-memory SoCs the shared
  DRAM/last-level cache is the dominant interference channel (as in MASK); the
  interference model weights bandwidth pressure heavily, and the scheduler avoids
  co-running two bandwidth-bound kernels across engines.
- **Offload decision**: for kernels that exceed the local budget, the scheduler
  chooses onboard vs offload by latency + energy + link availability (the
  offload/overload trade-off), treating a remote executor as another placement.

### 4.3 Use cases

- The VLA transformer runs on the DLA while the GPU does SLAM; the scheduler
  staggers their memory-heavy phases so neither starves the shared bus.

## 5. Energy-aware scheduling

### 5.1 Requirement

Hold deadlines within a power/thermal/payload envelope.

### 5.2 Mechanism

- **Precision/rate/detail as energy knobs**: quantize inference (int8/int4),
  lower non-critical rates, coarsen maps — recover power headroom for the hard-RT
  class.
- **DVFS + race-to-idle vs pace-to-deadline**: the scheduler chooses clocking per
  the deadline slack and thermal state.
- **KPIs include Joules/inference and sustained-power headroom**, not just
  latency/throughput.

### 5.3 Use cases

- On a battery-limited drone, the scheduler paces perception to stay under the
  thermal cap while guaranteeing the flight-control loop.

## 6. Mapping onto the orchestrator

- Robotics is a **mixed-criticality composition** of the orchestrator's classes:
  `CONTROL` (hard-RT, reserved + preempt), `ML_INFER`/`SCIENTIFIC` (perception,
  SLAM, planning — firm/soft, co-run + partition), with **energy** added as a
  first-class objective and **heterogeneous engine placement** as an extended
  partition tier.
- Levers reused: reservation + preemption (hard-RT), complementarity co-run
  (compute conv × memory-bound fusion), interference model weighted for
  unified-memory contention, admission/rate control, graceful degradation.
- New levers this workload adds to the orchestrator: **periodic/rate-based
  admission**, **engine-affinity placement (GPU/DLA/CPU)**, and **energy governor
  (precision/rate/DVFS)**.

## 7. Dependencies / risks

1. Requires the missing foundational + domain kernels: **sparse LA (SpMV /
   sparse solve for BA & pose-graph), structured-grid/stencil (flow, stereo,
   TSDF), scatter/hash (voxel hashing, histogram), nearest-neighbor gather (ICP),
   RNG + geometry (sampling planners), and the ML kernels (conv, attention) for
   perception/VLA** — a superset of the earlier gaps.
2. Hard-RT guarantees need predictable execution + preemption granularity the
   backend may not provide; reservation + measured WCET bound the risk.
3. Unified-memory contention modeling is hard; the online feedback loop calibrates
   it per platform.
4. Energy modeling per platform is empirical; treat as a measured governor input.

## 8. References

Corpus + markers in [`research/robotics-edge/papers/`](../research/robotics-edge/papers/):
cuRobo (GPU motion planning), MegBA (GPU bundle adjustment), real-time GPU depth
fusion (TSDF/mapping), a heterogeneous-edge-GPU scheduling survey, DARIS
(oversubscribed real-time DNN scheduling), holistic heterogeneous scheduling for
autonomous systems, an edge-robotics survey, energy-efficient edge-robotics
circuits/systems, OpenVLA (on-robot LLM-class policy), and a manipulation-workload
platform measurement study. Sibling specs:
[`xpu-kernel-scheduling`](xpu-kernel-scheduling-spec.md),
[`llm-kernel-scheduling`](llm-kernel-scheduling-spec.md),
[`xpu-gfx-streaming-geometry`](xpu-gfx-streaming-geometry-spec.md). The real-time
accelerator scheduling survey in `research/xpu-scheduling/papers/` also applies.
