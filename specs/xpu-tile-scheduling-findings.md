# Tile scheduling — research findings (2026-09-06)

The synthesis sections of five corpus readings done for the Tile scheduling family
([`xpu-tile-manifest`](xpu-tile-manifest-spec.md), [`xpu-tile-scheduling`](xpu-tile-scheduling-spec.md),
[`xpu-tile-workload-profiles`](xpu-tile-workload-profiles-spec.md)). Every number carries
its paper and silicon. The full per-paper extractions (mechanism, results, assumptions,
adopt/adapt/cannot, testable requirements) live beside each corpus as
`research/<corpus>/notes-2026-09-06.md`; the PDFs are gitignored and each has a
`.pdf.txt` marker with its URL. Claims marked **[X]** in the robotics section are the
reader's extrapolation, not paper-sourced.

| Corpus | Papers | Notes |
|---|---|---|
| `research/xpu-scheduling/papers/` | 19 | `research/xpu-scheduling/notes-2026-09-06.md` (two readings, A and B) |
| `research/llm-serving/papers/` | 12 | `research/llm-serving/notes-2026-09-06.md` |
| `research/robotics-edge/papers/` | 10 | `research/robotics-edge/notes-2026-09-06.md` |
| `research/gfx-scheduling/` | 11 PDFs + `SOURCES.md` (10 web sources) | `research/gfx-scheduling/notes-2026-09-06.md` |
| `research/sim-scheduling/` | 13 PDFs + `SOURCES.md` | `research/sim-scheduling/notes-2026-09-06.md` |

---

# Part A — GPU co-scheduling corpus, reading A (Orion, Paella, REEF, Salus, PipeSwitch, Clockwork, roofline, Volkov, MASK, RT survey)

### 10.2 Taxonomy table

| axis | class | mechanism / granularity | representative work | what it needs from hardware |
|---|---|---|---|---|
| **Preemption** | none (run-to-completion) | queue ordering only | Clockwork, TensorRT, most vendor stacks | nothing |
| | **thread-block / workgroup level** | drain or self-terminate at block boundary | Elliott, Chimera, Wang et al., **Effisha** (pure software, arbitrary block boundaries) | a compiler pass; no HW change |
| | **kernel level** | wait for kernel end, or evict queued kernels | Basaran, Tanasic, GCAPS, PREMA (TPU) | driver hook |
| | **thread level** | true HW context switch | Capodieci et al. on Pascal (EDF + CBS) | HW preemption |
| | **transactionization** | kernel split into idempotent transactions + snapshot rollback | Lee et al. (RTAS '18); REEF's reset scheme is the same family | compiler + memory snapshot |
| | **network-layer level** | preempt between DNN layers | SEPT (TPU) | SRAM partitioning |
| **Priority** | fixed-priority | RM / DM / NPFP | RTGPU, SHAPE, STGM, RT-MOT (NPFP^flex) | none |
| | dynamic-priority, temporal | EDF-like, virtual deadline | EDF-like, TimeWall, Sun et al., R-TOD | none |
| | dynamic-priority, spatial | priority realized by SM allocation | TimeGraph, CARSS | partitioning |
| | server / budget | Constant Bandwidth Server, credit-based | Capodieci, Pegasus (AccCredit/CoSched/AugC/SLAF), Hosseinimotlagh | budget accounting |
| | ticket / FIFO queue | timestamped tickets sorted into partition queues | FRED (FPGA), Lee et al. | none |
| | scored / heuristic | MapScore, utility-greedy, confidence-aware | DREAM, Heimdall, RT-MOT | none |
| | criticality-as-priority | mixed-criticality | PAAM (ROS 2), TherMa-MiCs | none |
| **Partitioning** | hardware, static | fixed HW partitions fixed at startup | **NVIDIA MIG**, Bakita et al., AMD FirePro/GRID | supported GPU only |
| | hardware, dynamic | reconfigurable regions | FRED / Cordone (FPGA DPR), Reshadi (systolic multi-tenant) | FPGA / systolic |
| | software-spatial (SM/CU assignment) | assign a count of SMs/CUs per task | RTGPU, STGM (Worst-Fit Decreasing guided by RTA), SHAPE, sBEET | needs SM steering, else FIFO+priority-boost fallback |
| | **temporal division (time slicing)** | tasks get exclusive time slices | PKM, TimeWall (two-level scheduler with **forbidden zones** so accelerator access cannot cross a slice boundary), E.[76] containers, RT-TAS | none |
| | **semi-temporal / quasi-partitioning** | slices further split, tasks migrate | CARSS, DART (DNN inference as pipeline stages), hQPS (MILP assignment + runtime migration) | none |
| | memory / cache partitioning | LLC or memory-access-path partitioning | Hoornaert et al., Roozkhosh et al. | HW or memory controller |
| **Deadline model** | **Accelerator-Only** | CPU time negligible; model only accelerator segments | RTGPU-adjacent analyses | — |
| | **Self-Suspension Segmented (SSSM)** | `τ_i = ((C¹,A¹,…,A^{M-1},C^M), D_i, T_i)`; CPU segments compute, accelerator segments are suspensions | TimeGraph, GPUSync, Elliott, RTGPU, SHAPE, STGM, EDF-like, ROSGM, SCENIC, BOXR | — |
| | **Resource-Included Model (RIM)** | `τ_i = (e_i^Re, T_i, D_i)`; one execution time per resource — the coarse case of SSSM | PKM, CARSS, TimeWall, sBEET, Capodieci, hQPS, RT-TAS, most perception work | — |
| | **DAG** | `τ_i = (V_i, E_i)` with per-vertex processor affinity; conditional nodes for alternative paths | Sun et al., DART, PAP*, Maity, TherMa-MiCs, FLEX | — |
| | **Task Chain** | `Γ_c = [τ_{c1},…,τ_{cn}]`, job *i+1* reads what job *i* wrote — emphasizes data dependency and communication latency | ROS/ROS 2 chains, Hazcat, PredJoule | — |


## 11. Cross-paper synthesis

### 11.1 Interference channels the corpus agrees dominate

Ordered by how much evidence there is, and by whether cajeta can act on them:

1. **Memory bandwidth, not compute.** Orion's decisive experiment is that two kernels each
   using only 40 % of SMs still fail to co-run (BN2d+BN2d, 1.08×) while a
   compute/memory pair wins 1.41×. Roofline explains why, and says the situation is worse
   now: ridge points have moved from ~1 flop/byte (Opteron X2) to ~80+ flops/byte (4090),
   so *almost every real kernel is on the bandwidth roof*. Volkov gives the quantitative
   handle: a device can only absorb `latency × bandwidth` bytes in flight (~100 KB on a
   GTX 480; far more now), and that budget is shared. **Actionable, and it is the primary
   scheduling signal.**
2. **Head-of-line blocking in finite FIFO hardware queues.** Paella's 18 %-of-a-GTX-1660
   result and REEF's 20–40 µs inter-stream dispatch delay are the same phenomenon from two
   sides. **Actionable: cap in-flight depth and choose what goes in.**
3. **Shared L2 / last-level cache and TLB.** MASK measures it at up to 45.2 % of achievable
   throughput in the multi-address-space case; REEF names it as the *residual* interference
   its CU partitioning cannot remove; Orion explicitly excludes it as future work. In a
   single address space the TLB half of this goes away, but the **cache-capacity half does
   not**. **Partly actionable: working-set-size admission plus non-temporal hints.**
4. **Host↔device transfer.** Clockwork: LOAD is 8.33 ms against a 2.61 ms INFER for
   ResNet50 — the transfer is *3× the compute*. PipeSwitch: 81–91 ms for ResNet152.
   Orion: the GPU cannot schedule kernels *at all* during a memcpy. The 2025 survey calls
   this newly non-negligible. **Actionable, and ~free on gfx1151's unified memory.**
5. **Process/context-switch tax.** MASK measures 8–12 % per process on real K40 and GTX
   1080 hardware; PipeSwitch measures 5.5–7.3 s of process/CUDA-context init.
   **Eliminated by construction in a single-process runtime — this is cajeta's structural
   advantage and should be stated as such.**
6. **The host scheduler itself.** Paella: injected scheduling delay degrades throughput
   from the low-µs region; Triton's non-CUDA overhead is up to 66 % of request latency.
   Salus rejected kernel-granularity scheduling partly on this ground. **Actionable, as a
   hard budget.**

### 11.2 Numbers worth pinning as budgets

| budget | value | source & silicon |
|---|---|---|
| **Scheduler decision cost** | **< 10 µs** per admission decision | Paella Fig. 9 (Xeon Silver 4114); REEF's own selection is 0.2–0.4 µs |
| **Instrumentation cost** | **≤ 6.6 µs** per kernel at 160 groups (aggregated ×16) | Paella, Tesla T4 |
| **Preemption / arrival-to-start** | **35–38 µs** with driver CU reset (AMD MI50); **71–288 µs** without (V100); **268–790 µs** wait-based | REEF |
| **In-flight device queue depth** | **4** descriptors; below 4 costs ~16 % exec time, above 4 costs linear preemption latency; CPU cost 17 %→31 % | REEF, MI50 |
| **Inter-stream dispatch delay** | **20–40 µs**; single kernel fetch+dispatch **~20 µs** | REEF, MI50 |
| **Duration-prediction error** | 99th pct over **144 µs** / under **55 µs** for compute; **431 / 348 µs** for transfers; compounds ~4× to ≈1 ms | Clockwork, V100 |
| **Kernel duration determinism** | 99.99th pct within **0.03 %** of median; per-kernel variance "a few µs" | Clockwork (V100) and REEF (MI50), independently |
| **Cost of uncontrolled concurrency** | **+25 % throughput for 100× tail latency** | Clockwork, V100 |
| **Kernel duration range to schedule at** | **10–255 µs** (inference); 100s–1000s µs (training) | REEF, Orion |
| **Best-effort throttle** | outstanding BE work **≤ 2.5 %** of protected budget; stable up to 3 %, linear degradation past it | Orion, V100 |
| **Context-switch tax if you go multi-process** | **8–12 % per process** (K40, GTX 1080, real HW) | MASK |
| **Task/context init if you go multi-process** | **5.5–7.3 s** | PipeSwitch, T4/V100 |
| **Achievable co-run outcome** | protected p99 within **9–22 %** of solo; aggregate throughput **1.3×–7.7×** | Orion (A100/V100), REEF (MI50) |
| **Bytes in flight needed to saturate bandwidth** | `latency × bandwidth`; **~100 KB** on GTX 480 (177 GB/s, <800 cyc) | Volkov |
| **Occupancy actually needed** | **8 %** for 87 % of pin bandwidth at 128 B/lane; **12.5 %** for ~100 % of ALU peak at ILP=4 | Volkov, GTX 480 |

### 11.3 Classification signals a scheduler needs per kernel

Merging all ten papers, the submit seam must carry (or the runtime must derive):

1. **Arithmetic character** — operational intensity relative to *this device's* ridge
   point, three-valued {compute, memory, indeterminate}. *(Roofline defines it; Orion
   proves it is the decisive signal; §7.5 R3 handles the near-ridge case.)*
2. **Resource footprint vector** — groups, lanes/group, vector registers, scalar
   registers, LDS/shared memory. Not occupancy. *(Paella Table 1; Volkov's whole
   argument; Orion's `sm_needed`.)*
3. **Expected duration + a predictability flag** — a rolling high-percentile per (kernel,
   geometry, backend), plus a boolean saying whether variance is small enough to schedule
   against. *(Clockwork's profiles; REEF's Rule 1 needs the duration; Orion's
   `DUR_THRESHOLD` needs it.)*
4. **Deadline window `(earliest, latest)`** — and the rule that a missed window is an
   error, not a retry. *(Clockwork.)*
5. **Read set and write set, complete at submit time** — which yields, for free:
   working-set size (MASK's cache channel), footprint for the Salus safety condition, and
   the handoff constraints of a task chain (survey).
6. **Restartability** — derived from read/write set disjointness. Only restartable kernels
   may be cancelled. *(REEF's idempotence assumption, made checkable.)*
7. **Priority class / schedule policy** — {latency, throughput, frame-budget} plus a
   protected/best-effort bit. *(Every system in the corpus.)*
8. **Degradation options**, where they exist — a cheaper variant the scheduler may
   substitute to hit a deadline. *(Survey: PAP*, RTScale, foveated rendering.)*

### 11.4 Where the papers contradict each other

- **Serialize versus co-run.** **Clockwork** measures that concurrency buys +25 %
  throughput at 100× tail latency and concludes: run **one** kernel at a time.
  **Orion, REEF and Paella** all conclude the opposite — co-run aggressively, and get
  1.3×–7.7×. The contradiction is not about facts; it is about **whether the protected
  kernel saturates the device**. Clockwork's ResNet50 at batch 16 on a V100 does; Orion's
  Table 1 shows MobileNetV2 inference at 6 % SM utilization does not. **Resolution for
  cajeta**: the decision is per-kernel, not global — serialize when the protected kernel's
  footprint already fills the device, co-run when it does not. The footprint vector is
  what decides, which is another reason it must be in the seam.
- **Scheduling granularity.** **Salus built a kernel-granularity scheduler and rejected
  it** (central bottleneck, breaks the framework's own kernel batching and pipelining,
  progressive-allocation deadlock) in favour of iteration granularity. **Orion, REEF and
  Paella all schedule at kernel granularity and it is the point.** Resolution: Salus's
  objections are all consequences of scheduling *underneath* an uncooperative framework.
  A runtime that owns the seam and gets complete buffer sets at submit time has neither
  the deadlock nor the broken-batching problem. But Salus's *cost* objection stands and
  becomes Paella's < 10 µs budget.
- **Preemption versus padding.** **REEF** argues you must be able to kill running work
  (35 µs) and pad the survivor's leftover CUs. **Orion** argues preemption is unavailable
  on the platforms that matter and you should instead *not submit* work you will regret,
  and beats REEF on the protected job's tail latency by 2.3–3× while getting the
  best-effort job much further than REEF does (REEF "barely executes the best-effort
  job"). Resolution: they optimize different things — REEF minimizes arrival-to-start,
  Orion minimizes *interference while running*. cajeta needs both, and Orion's own results
  show admission control is the cheaper half.
- **Occupancy.** **Volkov** says maximize work per lane and let occupancy fall to 8 %.
  **Orion/Paella/REEF** all use occupancy-derived footprint as an admission signal, and
  REEF's Rule 2 explicitly requires the best-effort kernel's *CU occupancy to be at least
  as high as* the real-time kernel's. These are compatible but easy to confuse: occupancy
  is a **capacity accounting unit** for the scheduler and a **non-goal** for the kernel
  author. Anything in cajeta that reports occupancy must say which of the two it means.
- **Where the model lives.** **Clockwork** consolidates every choice centrally and treats
  any deviation as an error. **Paella** distributes ground truth (the device tells the
  host where every block landed) and schedules reactively. Clockwork's approach needs
  prediction accuracy; Paella's needs a fast feedback path. cajeta can have both —
  Clockwork's windows for admission, Paella's notifications for correction — and the
  combination is stronger than either, because Clockwork's measured 4× error compounding
  is exactly what a feedback signal fixes.
- **Fairness.** Paella needs a deficit counter to stop SRPT starving long jobs; REEF's
  policy explicitly starves best-effort work to protect real-time; Orion sits between with
  a tunable throttle. There is no agreement, and the survey confirms there is no universal
  scheduler. This argues for cajeta's three named policies rather than one.

### 11.5 What is stale on 2024+ hardware

- **Volkov's constants (2010).** ILP=4 saturation, 192/576/864 ops per SM, ~100 KB in
  flight, ≈64 registers per thread, 6× register:LDS bandwidth gap. All of these must be
  re-measured on gfx1151 and AD102. The *mechanism* (Little's Law; more work per lane;
  registers are the only fast bandwidth) is more relevant than in 2010, not less — and
  RDNA3's dual issue makes ILP a **requirement** for peak rather than an alternative to
  occupancy, which is the GF104 situation Volkov flagged as unusual.
- **Roofline's ridge points (2009).** 1.0–6.7 flops/byte then; **~80+ flops/byte** on a
  4090 for FP32 and into the hundreds for tensor formats. The consequence is a change of
  regime, not a change of model: in 2009 the interesting kernels straddled the ridge; in
  2025 nearly everything is memory-bound, so opposite-character pairs are scarcer and each
  one is worth more. Also: "DRAM traffic filtered by the cache hierarchy" now means
  filtered by a 96 MB L2 (4090) or a MALL/Infinity Cache (RDNA3) — a much bigger filter
  than a 2009 L2.
- **MPS overhead numbers.** PipeSwitch's 193–338 ms and Salus's "MPS crashes TensorFlow"
  are CUDA 8–10.1 era. MPS has improved. The structural criticism — MPS gives you
  concurrency but no *policy* — is unchanged, and is exactly why every paper in this corpus
  had to build something on top of it.
- **"GPUs lack preemption."** Pascal added instruction-level preemption in 2016; REEF and
  Paella both note it exists but that there is no public software-controllable interface
  and the swapping overheads are prohibitive in practice. Effectively still true for a
  userspace runtime in 2025, so the papers' conclusions stand — but the reason is
  *interface availability*, not silicon, and it could change.
- **Datacenter-only mechanisms.** MIG (A100/H100), MPS's process model, and MASK's entire
  hardware proposal do not exist on gfx1151 or a 4090. Every partitioning idea cajeta can
  use is in the software-spatial or temporal-division column of §10.2.
- **Memory-capacity framing.** Salus's whole GPU-lane design is dimensioned around a 16 GB
  P100 where resnet152 at 13.8 GB nearly fills the device. On gfx1151 the capacity picture
  is completely different (large, unified, shared with the host); the *safety condition*
  survives, the constants and the urgency do not.
- **PCIe as the transfer bottleneck.** PipeSwitch (PCIe 3.0 ×8/×16) and Clockwork's 8.33 ms
  LOAD are both discrete-GPU facts. On gfx1151 they largely vanish; on a 4090 over PCIe 4/5
  they shrink but persist. Any transfer-pipelining machinery cajeta builds must be
  conditional on the backend reporting non-unified memory.
- **Not stale, and worth restating**: Clockwork's determinism measurement (99.99th
  percentile within 0.03 % of median) and REEF's independent confirmation (few-µs variance
  on a completely different vendor's hardware) are the foundation everything else rests on,
  and there is no reason to believe modern GPUs are less deterministic for branch-free
  fixed-geometry kernels. cajeta's cooperative-tile surface produces exactly that shape of
  kernel, so cajeta gets to build on the strongest result in the corpus.

---

# Part B — GPU co-scheduling corpus, reading B (iGniter, MISO, gpu-lets, Clipper, Nexus, AlpaServe, AntMan, TGS, Gandiva)

### 1.2 Formulas, reproduced exactly

Notation: workload `i ∈ I`, GPU `j ∈ J`. `r^{ij} ∈ [0, r_max]` is the
allocated SM fraction (`r_max = 1`). `b^i` is batch size.

**End-to-end latency and throughput**

    t_inf^{ij} = t_load^i + t_gpu^{ij} + t_feedback^i                      (1)

    h^{ij} = b^i / (t_gpu^{ij} + t_feedback^i)                             (2)

**Transfer phases** (`d_load^i`, `d_feedback^i` are byte sizes at
`b^i = 1`; `B_pcie` is measured host→device bandwidth)

    t_load^i     = d_load^i · b^i / B_pcie
    t_feedback^i = d_feedback^i · b^i / B_pcie                             (3)

**GPU execution phase, scaled by the frequency droop**

    t_gpu^{ij} = (t_sch^{ij} + t_act^{ij}) / (f^j / F)                     (4)

**Scheduling-delay term — linear in kernel count**

    t_sch^{ij} = (k_sch^i + Δ_sch^j) · n_k^i                               (5)

           ⎧ 0,                                    if Σ_{i∈I} v^{ij} ≤ 1
    Δ_sch^j = ⎨                                                             (6)
           ⎩ α_sch · Σ_{i∈I} v^{ij} + β_sch,       otherwise

           ⎧ 1   if workload i runs on GPU j  (r^{ij} > 0)
    v^{ij} = ⎨                                                              (7)
           ⎩ 0   otherwise                    (r^{ij} = 0)

**L2-contention term — the co-runners' solo L2 utilizations, summed**

    t_act^{ij} = k_act^i · ( 1 + α_cache^i · Σ_{i'∈I\i} ( c^{i'} · v^{i'j} ) )   (8)

**Power-cap frequency droop** (`P` = board power cap, `F` = max clock;
`α_f` is negative)

           ⎧ F,                                    if p_demand^j ≤ P
      f^j = ⎨                                                               (9)
           ⎩ F + α_f · ( p_demand^j − P ),         if p_demand^j > P

    p_demand^j = p_idle + Σ_{i∈I} ( p^i · v^{ij} )                        (10)

**Solo active time as a function of batch and allocated resource**
(quadratic in batch, inverse in resource)

    k_act^i = ( k_1^i·(b^i)² + k_2^i·b^i + k_3^i ) / ( r^{ij} + k_4^i ) + k_5^i  (11)

**Solo power and solo L2 utilization are linear in "GPU processing
ability" `b^i / k_act^i`**

    p^i = α_power^i      · b^i / k_act^i + β_power^i
    c^i = α_cacheutil^i  · b^i / k_act^i + β_cacheutil^i

**Closed-form batch size and lower-bound resource** (Theorem 1)

    b_appr^i = ⌊ T_slo^i · R^i · B_pcie / ( 2 · ( B_pcie + R^i · d_load^i ) ) ⌋   (17)

    r_lower^i = ⌈ γ^i / ( δ^i · r_unit ) − k_4^i / r_unit ⌉ · r_unit             (18)

    where  γ^i = k_1^i·(b_appr^i)² + k_2^i·b_appr^i + k_3^i
           δ^i = T_slo^i/2 − (d_load^i + d_feedback^i)·b_appr^i / B_pcie
                           − k_5^i − k_sch^i · n_k^i

Optimization: minimize `C = Σ_j u^j` s.t. `Σ_j h^{ij}·v^{ij} ≥ R^i`
(13), `Σ_j t_inf^{ij}·v^{ij} ≤ T_slo^i/2` (14), `Σ_i r^{ij} ≤ r_max`
(15), `Σ_j v^{ij} = 1` (16). The **half-SLO rule in (14)** — batch
execution latency may not exceed half the SLO — is inherited from Nexus
and is the reason batching and residency interact.

### 1.3 Required per-kernel / per-workload inputs

**8 workload-specific**: `d_load`, `d_feedback`, `n_k`, `k_sch`,
`k_act`, `p`, `c`, `α_cache`.
**7 hardware-specific**: `P`, `F`, `p_idle`, `B_pcie`, `α_f`, `α_sch`,
`β_sch`.
`d_load`, `d_feedback`, `n_k`, `k_sch` come from **one** Nsight Systems
trace. `α_f`, `α_sch`, `β_sch`, `α_cache` come from launching 2–5
workloads concurrently. `k_act`, `p`, `c` come from **11 configurations**
of (resource, batch) per workload, fit by least squares — vs. 40 × 32 =
1280 for exhaustive.


# Cross-paper synthesis

## A. The interference models, and what each demands per kernel

Four distinct models appear, at four price points.

| Model | Form | Inputs per kernel | Profiling cost | Co-runners | Reported accuracy |
| --- | --- | --- | --- | --- | --- |
| **TGS** (launch rate) | none — closed-loop AIMD on `α_in` | launched blocks per launch (from geometry) | **zero** | N | n/a; converges `O(B log B)`, 0.3–5% overhead |
| **Gandiva** (occupancy threshold + revert) | none — trial and revert | solo GPU utilization, solo memory | one solo run | 2 | none; explicitly declared unpredictable |
| **gpu-lets** (linear regression) | `Δ = c1·l2₁ + c2·l2₂ + c3·mem₁ + c4·mem₂ + c5` | solo L2 util, solo DRAM BW util | **1250 pairs / 2500 points** | **2 only** | 90% within 10.26%, 95% within 13.98% |
| **iGniter** (analytical, 3 channels) | Eq (5)–(11) above | `d_load`, `d_feedback`, `n_k`, `k_sch`, `k_act(b,r)`, `p`, `c`, `α_cache` | **11 configs**, ~4 min | **N** | 1.53–5.02% at 4 co-runners; 0.04–9.29% overall |
| **MISO** (learned MPS→MIG) | U-Net autoencoder | 3×7 progress-rate matrix from 30 s of probing | 2800 mixes offline | ≤7 | MAE 1.7%; tolerant to 9% |

**The per-kernel inputs cajeta already has for free at the submit seam,
that these papers had to profile:**

- `n_k` (launch count) — iGniter profiles it with Nsight Systems.
- Launched block count per kernel — TGS builds an interposition layer for it.
- Read/write buffer sets → **L2 working-set estimate `c`** — iGniter and
  gpu-lets both measure this with Nsight Compute / rocprof.
- Read/write byte volume → **DRAM bandwidth demand `mem`** — gpu-lets
  profiles it.
- Arithmetic-character tag (compute vs memory bound) — this is precisely
  the latent variable that gpu-lets' four-counter regression and MISO's
  autoencoder exist to recover. **Cajeta is told it. This is the single
  largest structural advantage over every system in this batch**, and it
  should collapse gpu-lets' 1250-pair profiling requirement to a
  per-backend one-time calibration over a synthetic kernel family.
- Static occupancy (registers, LDS, waves per CU) — known at compile time.

**What still must be measured at runtime, per kernel, no shortcut:**

1. Solo kernel duration as a function of geometry and CU count —
   `k_act(b, r)`, needed by iGniter Eq (11) and by the `p_eff` knee.
   Minimum viable: 3 CU levels × 2 geometries = 6 runs.
2. `α_cache` — the kernel's *sensitivity* to L2 pressure, which is not
   the same as its L2 footprint. Requires at least one co-run.
3. Power draw `p` — needed for Eq (9)/(10) and unavoidable at the
   ~10–100 ms sampling latency of `rocm-smi` / NVML.
4. `α_sch`, `β_sch`, `α_f` — per-backend constants, calibrated once.

**Recommended layering** (cheapest first, escalate only on failure):
TGS launch-rate AIMD as the always-on safety net → Gandiva's occupancy
threshold as the admission gate → iGniter's three-channel model to choose
the CU mask when a deadline is declared → gpu-lets' `p_eff` knee to cap
the mask. Do not build MISO's learned predictor; its purpose is to avoid a
reconfiguration cost cajeta does not pay.

## B. Batching-vs-latency rules that transfer to a single process

1. **The half-SLO rule.** Nexus and iGniter both enforce: batch execution
   latency ≤ SLO/2, because a request that misses one batch is served by
   the next. Generalized for a duty cycle over N resident workloads:
   worst case is **`duty_cycle + ℓ(b)`**, not `2·ℓ(b)` (Nexus §4.1).
   Any admission test that uses `2·ℓ(b)` under-provisions the moment a
   second workload is resident.
2. **`ℓ(b) = αb + β`** is the transferable cost model (Nexus Eq 1,
   Clipper's observed linearity). `β` is the launch-and-fixed cost; the
   whole point of batching is amortizing it. Cajeta can obtain `α` and
   `β` from **two launches**.
3. **Delayed batching pays exactly in proportion to `β/α`.** Clipper
   measured +3.3× for a high-`β` model and **0×** for a low-`β` one at the
   same 2 ms wait. Do not apply a global batch-wait timeout; gate it on
   the measured ratio.
4. **AIMD with a 10% multiplicative decrease** matched a fitted quantile
   regression (Clipper Fig. 4) and is robust to throughput shifts. Use it
   as the default for the throughput policy.
5. **Early drop beats lazy drop by up to 25%** (Nexus Fig. 9). Lazy drop
   (Clipper's policy) always serves the oldest request, therefore always
   picks a small batch, therefore falls further behind. Where dropping is
   legal (game frames, streamed video), drop the requests that would
   force a bad geometry, not merely the expired ones.
6. **Batching stops paying when a single item already saturates the
   device.** AlpaServe measured this for large models. The corollary:
   whether to batch is a function of the *occupancy* of the unbatched
   kernel, which cajeta knows statically. Below ~15% occupancy, batch
   aggressively; above ~85%, do not batch at all, interleave instead.
7. **Latency budgets across a pipeline must be split by optimization,
   not evenly.** Nexus's traffic pipeline optimum was **345 ms of 400 ms**
   to the first stage; an even split cost 19% throughput. The optimum
   depends on inter-stage fan-out `γ` and changes with the workload
   (Nexus Fig. 4: the best of three splits differs for γ = 0.1, 1, 10).
8. **CPU/GPU overlap is worth 7.4× in the tight-deadline small-kernel
   regime** and only 1.34× in the loose-deadline large-model regime
   (Nexus, game vs traffic ablations). Game rendering and LLM decode are
   the first regime. This is not a batching rule but it dominates every
   batching rule in that regime.

## C. Spatial-partition mechanisms on consumer AMD/NVIDIA, and what each buys

| Mechanism | Available on | In-process? | Granularity | Cost to change | What it buys | Evidence |
| --- | --- | --- | --- | --- | --- | --- |
| **HIP CU mask** (`hipExtStreamCreateWithCUMask`) | RDNA3 incl. gfx1151, CDNA | **yes** | per-WGP, per-stream | stream creation, µs | true spatial isolation of vector ALUs; L2/MALL and DRAM BW still shared | not evaluated by any paper here; closest analogue is MPS thread-percentage |
| **CUDA green contexts** (12.4+) | Ada incl. 4090, Hopper, Blackwell | **yes** | SM-partition units (~8 SMs on Ada) | context creation | same as above, in-process | post-dates all nine papers |
| **CUDA MPS `ACTIVE_THREAD_PERCENTAGE`** | Volta+ incl. 4090 | **no** (cross-process) | 1% nominal, coarser in practice | 10–15 s (process respawn, model load, warm-up) | what iGniter/gpu-lets/MISO actually used | gpu-lets: +102.6% throughput; iGniter: −25% cost |
| **MIG** | A100/A30/H100 only | n/a | 7 fixed slice types, 18 legal partitions | ~4 s GPU reset + checkpoint all jobs | hard SM + L2 + memory-BW + fault isolation | MISO: +16% JCT over best static; **TGS: loses to soft rate control** |
| **Stream priority** (`hipStreamCreateWithPriority`, `cudaStreamCreateWithPriority`) | both | yes | 2–3 levels, no resource guarantee | free | weak hint only; the driver still FCFS-arbitrates ready work | Nexus's negative result on driver FCFS applies |
| **Launch-rate throttling** (submit seam) | both, everywhere | yes | continuous | free | isolation without any hardware support | **TGS: 5–12.3% overhead, matches AntMan, 15× over MPS on the opportunistic job** |
| **Time-interleaving / suspend-resume** | both | yes | iteration boundary | ≤100 ms (Gandiva image models) | full isolation, zero interference | Gandiva: <2% aggregate loss at 60 s quantum |
| **Vulkan queue priority / async compute** | both | yes | queue | free | graphics/compute overlap only | not covered by these papers |

**The load-bearing conclusions.**

- **The absence of MIG is not a limitation.** TGS measured a hard MIG
  partition *losing* to soft launch-rate control — 77% of exclusive
  throughput for the protected job, because MIG's largest 2-way slice is
  4/7 of SMs and half the memory, and it cannot be resized while jobs
  run. Cajeta's soft mechanisms are strictly more flexible.
- **CU masking / green contexts recover almost everything MPS gave
  iGniter and gpu-lets**, at a repartition cost of microseconds instead
  of 10–15 seconds. This means the 20 s scheduling period (gpu-lets) and
  the epoch-scale provisioning (iGniter) are artifacts that should not be
  inherited; cajeta can repartition per frame.
- **Nothing available on consumer hardware partitions L2 or DRAM
  bandwidth.** Turing, Ada and RDNA3 all lack memory-bandwidth isolation
  (gpu-lets footnote 1 says Ampere introduced it; consumer parts did not
  inherit it). So iGniter's Eq (8) L2 term and gpu-lets' DRAM term are
  *mandatory*, not optional — spatial partitioning does not make them go
  away.
- **Rate control and spatial partitioning are complementary and no paper
  evaluates the combination.** TGS has rate control and no partitioning;
  iGniter/gpu-lets/MISO have partitioning and no rate control. Choosing a
  CU mask by the interference model and then trimming with AIMD is the
  obvious cajeta design and is an open question in the literature.

## D. Where the papers contradict each other

1. **Hard partition vs soft control.** MISO: MIG's hardware isolation
   beats MPS "in most cases" and dynamic MIG beats optimal static MIG by
   16%. TGS: MIG loses to software rate control on the production job
   (77% vs ~90% of exclusive) and loses catastrophically on the
   opportunistic job (TGS is 259× better in one case), because MIG's
   slices are fixed and cannot be resized while work runs.
   **Resolution:** MISO's workloads are long training jobs where a 4 s
   reset amortizes and both jobs are equal citizens; TGS's are a
   protected job plus a scavenger, where the protected job needs more
   than any fixed slice can give. Cajeta's case is TGS's case. **Prefer
   soft control; use partitioning as a coarse hint, not a guarantee.**

2. **Concurrent packing vs time-slicing.** Gandiva measures packing as
   **negative** (−11% to −16%) for high-occupancy models and refuses to
   model it ("predicting packing performance even with jobs of the same
   type appears challenging"). AntMan and TGS both co-run
   high-utilization models successfully (ResNet-50 retaining 57% while
   ESPnet holds its SLA; production job at 89–95% of exclusive).
   **Resolution:** Gandiva packs *without any rate control* and *without
   MPS* on pre-Volta hardware. The difference is not packing vs
   time-slicing, it is regulated vs unregulated co-residency. **Unregulated
   co-residency of two high-occupancy kernels is a loss; regulated
   co-residency is not.**

3. **Drop policy.** Clipper uses lazy drop (drop only after the deadline
   is missed) and determines batch size from the earliest request's
   remaining budget. Nexus measures this policy as up to 25% worse than
   early drop, and shows its bad rate reaching ~35% under Poisson arrival
   with a small `α`. **Nexus wins on measurement; Clipper is the earlier
   paper.**

4. **Batching's importance.** Nexus: batching is 4.7–13.3× and is the
   organizing principle of the whole system. AlpaServe: "we do find
   batching is helpful, but the gain is limited," and disables it for
   most experiments. **Resolution:** the difference is whether one
   request saturates the device. It does for a 6.7B-parameter forward
   pass on a V100; it does not for LeNet or ResNet-50 on a GTX 1080.
   The deciding variable is unbatched occupancy — which cajeta knows
   statically. *Also note this is where AlpaServe is most stale:
   continuous batching for LLM decode (2023+) re-established batching as
   the dominant lever for exactly the workload AlpaServe excluded.*

5. **What is observable.** iGniter builds its scheduler on the claim that
   GPU scheduling delay grows linearly and predictably with co-runner
   count and can be modelled from `n_k`. TGS argues the opposite about
   the closely related signal — "an empty production queue does not mean
   production jobs are not using the GPU... keeping track of GPU kernels
   running on the GPU is also not feasible, because the state of the GPU
   is not fully visible" — and explicitly rejects GPU utilization as a
   control signal. **Both can be right:** iGniter's `Δ_sch` is an offline
   *statistical* fit over co-runner count, not an online observation.
   Cajeta should use iGniter's fit for admission planning and TGS's
   arrival-rate signal for online control, never the reverse.

6. **Profiling cost.** gpu-lets: 1250 pairs (2500 data points), and the
   model still only handles 2 co-runners. iGniter: 11 configurations per
   workload, ~4 minutes, handles N. MISO: 2800 job mixes offline plus
   30 s online per mix. Gandiva: zero offline, pure trial-and-revert.
   TGS: zero, ever. **The measured accuracy ordering does not follow the
   cost ordering** — iGniter (11 configs) is more accurate than gpu-lets
   (1250 pairs) at 4 co-runners because it is analytical rather than
   regressed. Cheap and structured beats expensive and fitted.

7. **Spatial vs temporal as the primary axis.** Gandiva's entire design
   is temporal (60 s time slices, suspend-resume, migration), with
   packing as an opportunistic add-on. gpu-lets/iGniter/MISO's entire
   design is spatial, with temporal merging as an add-on. AntMan and TGS
   are neither — they regulate *rate*, which is a third axis both camps
   ignore.

## E. What is stale on 2024+ hardware

1. **"MPS is the only spatial partitioning knob."** False since CUDA 12.4
   (green contexts, in-process SM partitioning on Ada/Hopper/Blackwell)
   and false on ROCm (per-stream CU masks). Every "requires MPS, therefore
   requires multiple processes, therefore 10–15 s to repartition"
   conclusion in gpu-lets, iGniter and MISO is obsolete. **The 20 s
   scheduling period and the 4 s MIG reset are not costs cajeta pays.**

2. **Gandiva's packing table.** Measured **without MPS on P40/P100**
   (pre-Volta, no hardware concurrency support), and the authors say so.
   The negative entries (−11% to −16% for ResNet-50/ResNext-50/LSTM)
   should not be taken as a prediction for Ada or RDNA3. The *shape* —
   large gains below ~15% occupancy, losses above ~87% — is durable.

3. **MIG's relevance.** Still zero on consumer parts in 2024–2026 (no MIG
   on 4090, 5090, or any RDNA generation). MISO's entire output space is
   unreachable. Meanwhile MIG's *limitation* that TGS exposed — fixed
   slices, no live reconfiguration — persists on the datacenter parts.

4. **AntMan and TGS's memory-swap mechanism on APUs.** Both are built on
   "GPU memory is separate from and faster than host memory, so evict to
   host." **On gfx1151 the GPU and CPU share one physical LPDDR5X pool.**
   The compute half of both papers transfers cleanly; the memory half
   degenerates to "bound the allocator's retained cache," which is still
   worth doing (both papers show frameworks over-claim memory) but is no
   longer a scheduling lever. On a 4090 the original mechanism holds.

5. **AlpaServe's "batching gain is limited."** True for non-autoregressive
   large-model forward passes, which is what it measured, and explicitly
   scoped that way in the paper. **Stale for LLM decode**, where
   continuous batching is now the dominant throughput mechanism because
   decode is a memory-bound GEMV that becomes a GEMM under batching.
   Cajeta's LLM decode target must not inherit this conclusion.

6. **Clipper's "frameworks enforce static batch sizes."** Obsolete. But
   it raises a live cajeta-specific question: if cajeta specializes
   kernels per geometry, does an AIMD batch-size search trigger
   recompilation? If so, AIMD is unusable and the batch-size ladder must
   be quantized to pre-compiled geometries.

7. **PCIe as the transfer bottleneck** (iGniter Eq (3), Clipper's 1 GbE
   saturation, Nexus's overlap analysis). On gfx1151 there is no transfer.
   On a 4090 over PCIe 4.0 x16 (~25 GB/s effective) vs the paper's V100
   at 10 GB/s, the term is smaller but not gone.

8. **Power as a second-order effect.** iGniter treats the power/frequency
   channel as the third and smallest of three. On consumer parts it is
   arguably first-order: a 4090 throttles under mixed dense-FP32 +
   tensor-core load well before it runs out of SMs, and gfx1151 shares a
   package budget with the CPU so that host-side overlap (Nexus's 7.4×
   lever) *directly steals GPU clock*. **The Nexus overlap result and the
   iGniter power result interact on an APU in a way no paper in this
   batch measures, and that interaction should be an early experiment.**

9. **"4–5 CPU cores per GPU to saturate"** (Nexus). Modern host-side
   submission is cheaper (CUDA graphs, HIP graphs, persistent kernels),
   and on an APU the cores contend for the GPU's own memory bandwidth.
   Re-measure; do not inherit the constant.

---

# Part C — LLM serving and training corpus

# 13. Synthesis for the ML workload class

## 13.1 The phase model and the exact per-kernel roofline classes

### 13.1.1 The one algebraic fact that organizes everything

For a **weight-stationary GEMM** `[T, d] × [d, k]` with `T` tokens and weights stored at `b_w`
bytes/element, with `T ≪ d, k` so weight traffic dominates:

```
FLOPs      = 2·T·d·k
bytes      ≈ d·k·b_w
intensity  = 2·T / b_w        [FLOP/byte]        (derived)
```

Let `R = (device dense matrix FLOPS) / (achieved HBM bandwidth)`. The kernel is memory-bound
while `2T/b_w < R`, i.e. below

```
T_crossover = R · b_w / 2                        (derived)
```

**Three consequences, all load-bearing:**

1. **The roofline coordinate is total tokens in the launch, not batch size and not requests.**
   Sarathi says this in words ("the cost of the linear operation for 1 decode token is nearly the
   same as 128 prefill tokens"); DistServe says it as an engineering rule ("the number of new
   tokens in the batch is a reliable indicator of the batch's real execution time").
2. **The derivation matches the measurements.** A100-40GB: `R = 312 TFLOP/s ÷ 1.555 TB/s ≈ 200`,
   and with fp16 weights (`b_w = 2`) `T_crossover ≈ 200` — exactly Sarathi's stated theoretical
   figure ("we expect the operators to become compute-bound at ~200 tokens on A100 GPUs"). Their
   *measured* figure is **500–600 tokens** at TP-4, the gap being fixed per-launch overheads and
   collectives. DistServe independently measures `L_m ≈ 512` tokens for a 13B on one A100.
   **Rule: the practical crossover is 2.5–3× the algebraic one; measure it, don't compute it.**
3. **Quantization moves the crossover DOWN, not up.** With 4-bit weights (`b_w = 0.5`),
   `T_crossover = R/4`. On an A100 that is ~50 tokens algebraically, perhaps ~130–150 in practice.
   For cajeta-llm's quantized matvec kernels this is the most important derived result in this
   document: **the "free compute" that chunked prefill and speculative decoding spend does not
   exist in the same quantity for a Q4/MXFP4 model as for an fp16 one**, because the memory side
   already got 4× cheaper. Any policy tuned against fp16 literature will over-admit work.

### 13.1.2 Attention is a different operator in each phase — by three orders of magnitude

FlashAttention's traffic is `Θ(N²d²/M)` for `4N²d` FLOPs, so **prefill attention intensity ≈
`4M/d`** — with `M ≈ 100 KB` of SRAM/LDS and `d = 128` that is **≈3000 FLOP/byte**, i.e.
compute-bound by roughly 20× on any current device. Sarathi confirms empirically: "even at small
chunk sizes the attention prefill operation is compute bound."

**Decode attention** reads the whole KV cache to serve `g` query heads (GQA group size) per KV
head: `FLOPs = 4·L·d·g`, `bytes = 2·L·d·b_kv`, so **intensity ≈ `2g/b_kv`** — for fp16 KV and
`g = 8`, **8 FLOP/byte**, memory-bound by 20–40×. FlashInfer states the general form:
`I = O(1/(1/l_qo + 1/l_kv))`, which in serving simplifies to **`O(g · l_qo)`** — *query length
alone*, and **batching does not change it**.

So the **same operator swings ~400× in arithmetic intensity** purely from `l_qo`. A submit seam
that carries only "this is attention" cannot classify it; one that carries `(l_qo, l_kv, g)` gets
it for free, and gets speculative decoding's `l_qo = γ+1` right without a special case.

### 13.1.3 Per-kernel class table for one decoder layer

| # | Kernel | Prefill (T = chunk, e.g. 512–2048) | Decode (T = batch, e.g. 1–64) | Notes |
|---|---|---|---|---|
| 1 | RMSNorm / LayerNorm | memory-bound | memory-bound, **launch-bound** | fuse into neighbour |
| 2 | QKV projection GEMM | **compute** above `T_crossover` | **memory** | ~80% of layer time (Sarathi) |
| 3 | RoPE | memory | memory, launch-bound | fusing into attention: **1.6–3.7× BW util** (FlashInfer) |
| 4 | KV-cache scatter write | memory (pure) | memory (pure), tiny | fuse with reshape (vLLM kernel #1) |
| 5 | Attention | **compute**, ≈3000 FLOP/byte | **memory**, ≈`2g/b_kv` ≈ 8 | the 400× swing |
| 6 | Attn out projection GEMM | compute | memory | same rule as #2 |
| 7 | MLP gate/up GEMM | compute | memory | largest weights |
| 8 | SwiGLU / GeLU | memory | memory, launch-bound | Megatron: duplicate, don't communicate |
| 9 | MLP down GEMM | compute | memory | |
| 10 | Residual adds | memory | launch-bound | |
| 11 | (MoE) router GEMM | memory | memory, launch-bound | `O(d·E)`, negligible FLOPs |
| 12 | (MoE) dispatch gather / combine scatter | memory (pure) | memory, launch-bound | permutation, not a matmul |
| 13 | (MoE) `E` expert GEMMs | compute if `T·k/E > T_crossover` | **launch-bound swarm** | see §13.4 |
| 14 | LM head `[T,d]×[d,V]` | compute | **memory**, biggest single weight read | `V` = 32K–150K |
| 15 | Sampling / argmax | memory | launch-bound, µs-scale | |

**Phase summary with the measured evidence:**

- **Prefill = compute-bound, saturates at batch ≈ 1–2.** Splitwise: prompt-phase throughput
  *decreases* after 2048 prompt tokens. DistServe Fig. 3: prefill throughput is flat in batch size
  for inputs ≥ 512. Sarathi Fig. 3: prefill throughput saturates at batch size 1 (~4–5K tok/s,
  Mistral-7B, A100). Splitwise: prompt-phase power rises toward TDP and prefill latency is highly
  sensitive to power caps.
- **Decode = memory-bound, scales ~linearly with batch until residency runs out.** Sarathi:
  ≈10 → 100 → 200 → 420 → 800 tok/s at batch 1/8/16/32/64. Splitwise: "with a batch size of 64,
  there is only 2× impact on TBT" — 64× the work for 2× the time. Splitwise: token-phase latency
  is *unchanged* by a >50% power cap (700 W → 350 W). A100→H100 (3.43× compute, 1.64× bandwidth)
  buys prefill 0.51× TTFT but decode only 0.70× TBT: **decode tracks the bandwidth ratio.**
- **Decode does eventually become compute-bound.** DistServe: "as the decoding batch size continues
  to increase, the decoding computation begins to resemble the prefill phase"; paged attention and
  GQA "enable further scaling of the decoding batch size to nearly compute-bound." The crossover is
  `T_crossover` from §13.1.1 applied to the batch: **fp16 weights ⇒ ~200 (A100 theory) / ~500–600
  (measured, TP-4); 4-bit weights ⇒ roughly a quarter of that.**

## 13.2 Chunked prefill and continuous batching as single-process scheduling policies

Both are *request* policies in the literature, but each reduces to a small set of facts the
**kernel** scheduler must be able to see, and a small set of things it must be able to do.

### 13.2.1 What the scheduler must see

| Input | Source | Why |
|---|---|---|
| total token count `T` for the iteration | caller | the roofline coordinate (§13.1.1) |
| per-item `(l_qo, l_kv)` | KV manager | attention class + load balance (FlashInfer `α·l_q + β·l_kv`) |
| GQA group `g`, head dim `d`, head counts | model | effective query length `g·l_qo` |
| per-kernel **tile granularity** per dimension | compiler | 257 vs 256 costs **+32%** (Sarathi) |
| measured `T_crossover` / `L_m` per (kernel, device) | profiling pass | admission and co-run decisions |
| token budget `τ` from the latency target | policy | Sarathi: **512 strict, 2048 relaxed, 1536 for PP** |
| resident KV occupancy and free slots | KV pool | admission; deadlock avoidance (Orca) |
| chunk index / prior-chunk length sum | scheduler state | the `O(N²)` KV re-read term (DistServe) |
| whether an op is token-axis-flattenable | compiler | Orca's batching legality predicate |

### 13.2.2 What the scheduler must be able to do

1. **Build a launch from non-uniform work items.** Orca already did this in 2022 by "concatenating
   all thread blocks of the kernels for different requests… which is often discouraged by CUDA
   programming practice… we find this fusion to be beneficial by improving GPU utilization and
   reducing the kernel launch overhead." FlashInfer formalized it as a work queue of
   (query-tile, KV-chunk) items assigned to CTAs by a cost-balanced priority queue. **This is the
   single most important capability for the cajeta submit seam**: one launch, many geometries.
2. **Round every chunk to tile granularity.** Non-negotiable: +32% for a one-token miss.
3. **Hold a per-iteration cost budget.** Sarathi's `τ` is exactly a frame budget applied to a token
   count. The scheduler fills the budget decode-first, then partial prefills, then new work.
4. **Refuse to admit a whole prefill into a decode iteration.** Measured penalties for doing so:
   **up to 28.3×** TBT (LLaMA2-70B, ctx 4096, batch 1) and **20.3×** (Mistral-7B, ctx 4096,
   batch 1) versus **1.2–3.7×** for a chunked prefill. On a 13B DistServe measures **3–6×** for a
   *single* added prefill.
5. **Partition compute units, not just batches.** DistServe's interference numbers say that mixing
   phases into one launch inherits the compute-bound kernel's latency. FlashInfer's answer is that
   the SM count is a *plan input*: "this SM number can be provided by the user through the plan
   functions and the FlashInfer load-balancing scheduler will allocate tiles accordingly." The
   cajeta submit seam should carry a **compute-unit budget per launch**, and the tile planner must
   re-plan against it.

### 13.2.3 Numeric budgets to build into the spec

- Token budget `τ`: **512** (strict latency), **2048** (relaxed), **1536** where pipeline bubbles
  matter. Chunk overhead: **≤25% at chunk 512, ≈0 at chunk 2048** (Yi-34B, TP-2, prompts 2–8K).
- Prefill saturation cap: **~2048 prompt tokens** (Splitwise, BLOOM-176B/Llama2-70B on H100);
  **~512 tokens** (DistServe, 13B on one A100). Both are "the point where more batching stops
  helping" — measure per model+device, do not hard-code.
- Latency SLOs actually used, as a calibration for what "strict" means: Mistral-7B P99 TBT
  **0.1 s / 0.5 s**; Yi-34B **0.2 s / 1 s**; LLaMA2-70B and Falcon-180B **1 s / 5 s** — defined as
  5× and 25× the execution time of a decode iteration at 4K prefill, batch 32. DistServe's
  application SLOs: chatbot 13B TTFT 0.25 s / TPOT 0.1 s; code completion 66B 0.125 s / 0.2 s;
  summarization 66B 15 s / 0.15 s.
- Real workload shapes to test against: ShareGPT prompt median **1730** / P90 5696, output median
  **415**; arxiv-summarization prompt median **7059** / P90 12985, output median 208 (Sarathi).
  Azure production: coding prompt median **1500**, output median **13**; conversation prompt
  **1020**, output **129** (Splitwise). ShareGPT avg 755.5-in / 200.3-out, HumanEval 1713/98.2,
  LongBench 1738.3/90.7 (DistServe). **Prompt:output runs 4:1 to 115:1 — prefill is never a
  rounding error.**
- Real batch occupancy: **60–70% of the time fewer than 20 active tokens; >20% of the time a single
  token** (Splitwise, production traces). Design the decode path for `T ≤ 20`, not for `T = 64`.

## 13.3 KV-cache residency as a scheduler-visible resource

KV is unlike a launch's transient working set in four ways, all of which the submit seam's
read/write buffer sets cannot express today: it is **large**, **long-lived across many launches**,
**growing during the sequence**, and **shareable by reference count**.

**Sizes.** Per token, `KV_bytes = 2 · n_kv_heads · d_head · n_layers · b_kv`.
- OPT-13B (MHA, fp16): **800 KB/token** → a 2048-token request needs **1.6 GB** (vLLM).
- OPT-66B: a single 512-token request = **1.13 GB** (DistServe).
- A GQA-4 8B model (8 KV heads × 128, 32 layers, fp16): **128 KB/token** → 8K context ≈ **1 GB**
  (derived; this is the cajeta-llm-relevant regime).
- Budget on a 40 GB A100 with a 13B model: 26 GB weights, **12 GB KV = 15.7K slots**. 8×A100-80GB
  with 175B: 346 GB weights, **264 GB KV = 60.1K slots** (vLLM Table 1).

**Why it must be scheduler-visible, not allocator-visible:**
- **It sets the batch size, and therefore the roofline class.** vLLM raised the resident batch from
  7.00 → **30.42** (ShareGPT) and 7.00 → **132.44** (Alpaca) for OPT-13B purely by defragmenting.
  Splitwise: token-phase throughput "keeps increasing with batching until 64 batch-size, at which
  point the machine runs out of memory."
- **Naive reservation wastes 60–80%.** Effective utilization measured at **20.4%** (Orca-Max),
  26.8%, 38.2% (Orca-Oracle) vs **96.3%** (vLLM).
- **Admission must be checked, or the scheduler deadlocks.** Orca: reserve `max_tokens` slots at
  admission; "a naïve implementation can make the scheduler fall into a deadlock when the scheduler
  cannot issue an iteration for a new request… because there is no space left for storing a new
  Attention key and value."
- **Eviction is gang-structured.** vLLM's all-or-nothing per-sequence eviction, with sequence
  *groups* (beam candidates) gang-scheduled.
- **Recompute vs spill is a measured choice.** Recompute cost is constant in block size and "never
  higher than 20% of swapping's latency"; swapping wins only at large blocks; 16–64 is a wash.
- **Indirection is not free and is bounded to one kernel.** Paging costs **20–26% on the attention
  kernel only** — "the overhead is small as it only affects the attention operator but not the
  other operators in the model, such as Linear."
- **Block size couples the memory manager to the subgroup width.** vLLM assigns **one warp per
  block** and defaults to **block size 16**; 16–128 is fine for long sequences, but for short
  sequences (Alpaca) anything above 32 "significantly degrades" performance.
- **The scope warning matters for the multimodal mandate.** vLLM: "in DNN training, the tensor
  shapes are typically static… in serving DNNs that are not LLMs, an increase in memory efficiency
  may not result in any performance improvement since the performance is primarily compute-bound.
  In such scenarios, introducing vLLM's techniques may rather degrade the performance." **Paging is
  a per-workload policy, never a device-wide mechanism.**

**Requirement shape:** the cajeta seam needs a `ResidentPool` concept alongside buffer sets:
capacity, block size, occupancy, per-item reservation, per-item block list, refcounts, gang
membership, and a per-item recompute cost so eviction can be decided.

## 13.4 MoE routing's effect on kernel shape and load balance

MoE replaces one large dense GEMM with **one tiny router GEMM + a permutation + `E` ragged GEMMs +
an inverse permutation**. The scheduler-visible consequences:

- **Shape is data-dependent and known only at runtime, on-device.** The realized token count per
  expert `f_i·T` exists after the router and before the expert GEMMs. Switch's TPU implementation
  had to pad to a static **expert capacity** `C = (tokens_per_batch / E) × capacity_factor`; a
  single-process GPU scheduler does not, and should not — it should take the launch bound from the
  device-side count (indirect dispatch).
- **Capacity factor is a real throughput knob worth ~14%**: Switch-Base 1000 examples/s at CF 1.0
  vs 860 at CF 2.0 (32 TPUv3 cores, FLOP-matched). And it is **not monotone**: MoE-Base got
  *slower* going CF 2.0 → 1.25 (840 → 790). Measure, don't reason.
- **Dropped tokens are the imbalance metric**: **<1%** with the aux loss `α·N·Σf_i·P_i` at
  `α = 10⁻²`, independent of the number of experts. The kernel-level analogue is "fraction of a
  launch's work items that did not fit the planned tiles."
- **At decode batch sizes MoE is a launch-bound swarm.** With `T = 20` active tokens (Splitwise's
  measured typical), top-1 routing and `E = 64` experts, the mean expert sees **0.3 rows**. Each
  expert GEMM is `[~0–2, d] × [d, d_ff]` — a pure weight read, memory-bound and dominated by fixed
  overhead. **This is the worst launch-overhead case in the entire workload class** and is the
  strongest argument for (a) a grouped/batched GEMM primitive taking per-group offsets, and
  (b) co-scheduling, since none of these kernels can fill the device alone.
- **The dependency chain is strict per layer**: router → dispatch → experts → combine → next layer.
  Only the `E` expert launches are mutually parallel. Overlap must come from *other requests* or
  *other model stages*, not from within the layer.
- **Precision boundaries are placement constraints.** Switch's selective precision keeps the router
  in fp32 *locally* and casts back to bf16 before the all-to-all; doing it globally cost **1390 →
  1160 examples/s (≈17%)**. A fusion or retiling pass must not widen an fp32 region across a
  dispatch boundary.
- Routing cost itself is negligible: `O(d_model × E)`.

## 13.5 Speculative decoding's draft/verify overlap

- **The mechanism is a memory-to-compute conversion.** Leviathan states it directly: the target
  model's weights and KV "can be read **once per execution** of Algorithm 1, so the number of memory
  accesses for reading them shrinks by a factor of `(1 − α^{γ+1})/(1 − α)`", while total arithmetic
  rises by only `(1−α)(γĉ+γ+1)/(1−α^{γ+1})`. Measured trade at `c = ĉ = 0`: **1.11–1.63× more ops
  for 1.96–6.86× less wall time**.
- **Measured end-to-end**: T5-XXL (11B) with a T5-small (77M) draft on a **single TPU-v4**, batch 1:
  **3.4×** (EnDe, temp 0, γ=7, α=0.75), 2.6× (temp 1), **3.1×** (CNN/DM, temp 0, γ=5, α=0.65).
  Chinchilla 70B independently: 2–2.5×.
- **Within one request there is NO draft/verify overlap.** The draft chain is `γ` strictly serial
  steps, and the verify needs all `γ` guesses. Overlap exists only **across requests**: request A's
  verify (a mini-prefill, compute-heavy) against request B's draft chain (tiny, memory- and
  launch-bound). That is a textbook complementary pair.
- **The draft chain is the launch-overhead stress test.** `γ = 5–10` serial forward passes of a
  model "around two orders of magnitude smaller" than the target, at batch 1. Per accepted-token
  group that is hundreds of kernel launches for a few hundred microseconds of real work. **A
  capture/replay (graph) path or a persistent kernel for the draft chain is a prerequisite, not an
  optimization.**
- **The verify launch is a `γ+1`-query prefill.** With GQA `g = 8` and `γ = 7`, the effective query
  length is 64 — tensor-core tile territory — versus 8 for a plain decode step. `γ` alone changes
  the kernel's class, its tile size, and its parallel axis.
- **The committed work is data-dependent.** The accepted count `n` is known only post-verify;
  the KV commit and the next draft's start state are dynamic. Needs indirect launch bounds or a
  predicated worst-case launch.
- **`γ` must be tuned online**: optimal `γ` from `α` and the draft/target cost ratio `c` — at
  `c = 0.05`, α = 0.7 → γ ≈ 4; α = 0.9 → γ ≈ 10–12. Acceptance rates measured: 0.62–0.75 (T5-small
  draft for T5-XXL), 0.88–0.89 (6M draft for a 97M target), 0.57–0.75 (LaMDA 100M–8B drafts for
  137B), 0.19–0.20 (bigram).
- **The scope condition is the same predicate the scheduler already needs**: "our method is not
  helpful for configurations where additional computation resources are not available. However, in
  common cases where additional computation resources are available (e.g. when memory bandwidth is
  the bottleneck) our method provides the speedup." *So: any launch the scheduler has tagged
  memory-bound has, by construction, spare compute to speculate into — and §13.1.1's quantization
  result says that spare compute shrinks by ~4× for a Q4 model.*

## 13.6 What TTS/STT and CNN pipelines add — **[not from these papers; my own knowledge]**

**No paper in this set covers speech or vision.** The following is engineering knowledge, flagged
as such, and the numbers are order-of-magnitude, not citations.

### 13.6.1 STT / ASR

- **Whisper-class**: a conv front-end (2 × Conv1d, stride 2) over an 80-mel × 3000-frame (30 s)
  window, then a transformer *encoder* over 1500 states — this is a **prefill-shaped,
  compute-bound, single-shot launch** with fully static geometry. The *decoder* is autoregressive
  with **cross-attention over a fixed 1500-state KV that is computed once and never grows** — a
  third KV regime, distinct from the growing self-attention KV, and one that a resident-pool
  abstraction must model (static, shared, read-only for the whole utterance).
- **Streaming ASR** (conformer/RNN-T class): the encoder is chunked (typically 160–960 ms of audio,
  i.e. 16–96 frames at a 10 ms hop) with causal or chunked attention. Each chunk arrives on a
  **hard period** equal to its duration; the real-time factor must stay below 1 with margin. This
  is the workload that genuinely needs the **frame-budget** policy, and it is the only one in this
  document with true periodic deadlines.
- Kernel character: small, static, many. A conformer block at chunk 32 frames is a `[32, 512]`
  activation — deep in the launch-bound regime for every GEMM. Static shapes make graph capture
  trivially applicable, unlike LLM serving's ragged dynamism.

### 13.6.2 TTS

- Two stages with opposite characters. **(1) Acoustic model**: either autoregressive
  (Tacotron/VALL-E/Bark class, ~50–90 mel frames/s, decode-shaped, memory- and launch-bound) or
  diffusion/flow-matching (a *fixed* count of 20–50 denoising steps over the whole utterance, each
  a medium-size compute-bound U-Net/DiT launch — a predictable, schedulable block of compute with
  no data-dependent length). **(2) Vocoder** (HiFi-GAN class): transposed convolutions, memory-bound
  at streaming chunk sizes, embarrassingly parallel over time when batched.
- **Hard budget**: 24 kHz audio at a 256-sample hop = 93.75 frames/s = **10.7 ms per frame**.
  A conversational agent's mouth-to-ear budget is ~200–300 ms end-to-end, which must cover
  STT + LLM decode + TTS. Since the LLM decode in this stack is itself ~12 ms/token (cajeta-llm on
  gfx1151), the three pipelines *must* share one GPU under a deadline policy rather than each
  owning it.
- Scheduling shape: TTS and STT are **producer/consumer with periodic deadlines**, the LLM is
  **best-effort throughput**. Co-running them is the frame-budget policy's reason to exist:
  the LLM's memory-bound decode is exactly the workload that can be preempted or throttled to let a
  10.7 ms vocoder frame land, and the vocoder is exactly the compute-bound work that fills the
  decode's idle ALUs.

### 13.6.3 CNN vision

- **Convolutions are compute-bound at training batch sizes and often memory-/launch-bound at
  batch 1 inference.** A ResNet-50 forward at batch 1 is ~50 conv launches of tens to a couple of
  hundred microseconds; **depthwise-separable** convs (MobileNet/EfficientNet class) are memory-bound
  nearly everywhere, because a depthwise kernel does ~`k²` MACs per weight element with almost no
  reuse.
- **ViT-style vision encoders are prefill-shaped**: one transformer forward over 196–1024 patch
  tokens, compute-bound, single-shot, static geometry — the same class as an LLM prefill chunk, and
  therefore the same complementary partner for LLM decode.
- What vision adds to the requirements: (i) **NCHW/NHWC layout and im2col/implicit-GEMM choice** is
  a per-launch tiling decision with the same tile-quantization sensitivity as the LLM GEMMs;
  (ii) **static shapes** ⇒ the whole graph can be planned once; (iii) at batch 1 the pipeline is
  dominated by fixed overhead, so **fusion and capture matter more than tiling**.

### 13.6.4 The common additions

Speech and vision pipelines add four things the LLM papers do not exercise:
1. **Hard periodic deadlines** (10.7 ms audio frames, 16.7/33.3 ms video frames) → the
   frame-budget policy needs real preemption or at least admission that respects a deadline.
2. **Many small launches with fixed, statically-known shapes** → launch overhead is first-order and
   capture/replay is cheap to apply. **[my own knowledge]** an un-captured HIP/CUDA launch costs
   ~3–10 µs end-to-end with ~2–5 µs of unavoidable gap; graph replay drops per-node overhead to
   ~0.5–2 µs. Against a 10.7 ms frame budget, 300 un-captured launches is ~1–3 ms — 10–30% of the
   budget spent on nothing.
3. **A third KV/state regime**: static cross-attention KV (ASR decoder), and streaming ring buffers
   (conformer left-context, vocoder overlap-add state) that are neither transient nor growing.
4. **Multiple concurrent models in one process** — STT, LLM, TTS, and a vision encoder are four
   distinct weight sets resident at once. The residency accounting in §13.3 must cover weights as
   well as KV.

## 13.7 What training adds

### 13.7.1 The backward pass

- **~2.5× the forward FLOPs**: FA2 counts **5 matmuls in the backward vs 2 in the forward**.
  Forward attention FLOPs `= 4·seqlen²·head_dim·num_heads` (halved for causal).
- **Different parallel axis and different sharing.** FA2 parallelizes the forward over *row* blocks
  with no cross-block communication (embarrassingly parallel, freely splittable), and the backward
  over *column* blocks **with atomic accumulation into `dQ`**. Consequence for the submit seam:
  **the write set must distinguish exclusive-write from accumulating-write**; two launches that
  accumulate into the same buffer cannot co-run, and the result is order-dependent in float.
- **Fusion legality changes.** FlashAttention: "in the context of model training, the intermediate
  values still need to be written to HBM to save for the backward pass, reducing the effectiveness
  of naive kernel fusion." The survey says the same from the other side: "since the backward
  computation is not required for LLM inference, more kernel fusion chances exist." **The compiler
  must tell the scheduler whether an intermediate is live into a backward pass.**
- **Recomputation is the standard answer and it is a real scheduled launch.** FlashAttention
  recomputes `S`, `P` on-chip from `(Q,K,V,m,ℓ)`; Megatron applies **activation checkpointing after
  every transformer layer**. Both trade FLOPs for residency, and both must appear in the schedule
  with their own cost and buffer reservation — never as free.

### 13.7.2 The optimizer step

**[not from these papers; my own knowledge, flagged]** Adam is pure elementwise streaming over every
parameter: read `p, g, m, v`, write `p, m, v`. In fp32 that is 16 bytes of state per parameter and
~24 bytes of traffic per parameter per step. For a 1B-parameter model: **~24 GB of traffic per
step**, which at 200 GB/s is **~120 ms** — comparable to or larger than a small model's whole
forward+backward. It is **unconditionally memory-bound**, it has **no dependencies among
parameters**, and if issued naively it is *thousands of tiny launches* (one per tensor). Two
requirements follow: it must be expressible as **one fused multi-tensor launch**, and it is the
ideal co-run partner for anything compute-bound.

### 13.7.3 Gradient reduction overlap — Megatron's rules

- **Launch each parameter's reduction as soon as its gradient is complete**, not at the end of the
  backward: "During back propagation we run **multiple gradient all-reduces in parallel** to reduce
  weight gradients within each distinct data parallel group." The scheduler must track
  **per-parameter gradient readiness** and be able to issue a reduction/transfer on a queue that
  runs concurrently with the next backward kernel.
- **Choose partition axes so no synchronization is needed between fused GEMMs.** Megatron splits the
  MLP's first weight **column-wise** precisely so the elementwise GeLU needs no cross-partition sum,
  then splits the second **row-wise** so it consumes the local output — one all-reduce forward, one
  backward, **4 collectives per transformer layer per fwd+bwd**. Same reasoning as FA2's choice to
  split Q rather than K,V across warps: *which operand axis is partitioned determines whether the
  next op needs a cross-group reduction.*
- **Duplicate cheap memory-bound work rather than communicate it**: "Rather than having one GPU
  compute part of the dropout, layer normalization, or residual connections and broadcast the
  results to other GPUs, we choose to **duplicate the computation across GPUs**." Generalized
  in-process: *recompute a memory-bound elementwise op instead of synchronizing around it.*
- **Shrink the collective's payload by fusing it with its producer**: the logit GEMM fused with
  cross-entropy turns a `b×s×v` all-gather (v = 51,200) into a `b×s` all-reduce.
- **Pad to tile granularity**: "it is beneficial for the per-GPU vocabulary size to be a multiple of
  **128**", hence 50,257 → 51,200. Same class of constraint as Sarathi's 257-vs-256 +32%.
- **RNG stream identity is a correctness property**: dropout inside a model-parallel region must
  draw different numbers per worker; dropout outside must draw identical ones. Reordering launches
  must not change which stream a kernel draws from.

### 13.7.4 Realistic training efficiency targets

- Megatron single-V100 baseline: **39 TFLOPs = 30% of peak**, called "a strong single GPU baseline."
- FA2 end-to-end GPT training on 8×A100-80GB: **196–225 TFLOPs/s per GPU = up to 72% MFU**, and the
  long-context row is the telling one — at **8K context the un-optimized baseline collapses from
  142 → 72 TFLOPs/s while FA2 rises from 196 → 220**.
- FA2 attention kernel alone: **50–73% of A100 peak** forward, up to 63% backward; FA1 was 30–50%
  and 25–35%; a tuned GEMM is 80–90%. **A100 non-matmul FP32 peak is 19.5 TFLOPs/s vs 312 for
  fp16 matmul — one non-matmul FLOP costs ~16 matmul FLOPs.**

## 13.8 Consolidated submit-seam metadata (what every launch must carry)

Derived from all twelve papers. Items marked ★ are ones a naive design would omit and that a
measurement in this document proves are needed.

**Geometry**
- work-item list with **per-item geometry** (not one uniform shape) ★ — Orca, FlashInfer
- total token count `T` (the roofline coordinate) — Sarathi, DistServe
- per-item `(l_qo, l_kv)`, head counts `H_qo/H_kv` (⇒ GQA group `g`), head dim `d` — FlashInfer
- chosen tile shape and the **tile granularity per dimension** ★ — Sarathi (+32%), Megatron (128)
- implied workgroup count, to compare against the CU count ★ — FA2 ("≥80" on 108 SMs)
- per-group register + LDS footprint, checked against the target's budget ★ — FA1 (`M`), FA2
  ("the kernel cannot run at all")
- mask pattern (dense / causal / block-sparse / tree) and its per-row nonzero counts ★ — FA2, FlashInfer

**Buffers**
- read set, **exclusive**-write set, and **accumulating**-write set as distinct categories ★ — FA2 (`dQ` atomics)
- resident-pool handles with occupancy, block size, per-item reservation, refcounts, gang membership ★ — vLLM, Orca
- per-region (per-layer) output slices so consumers can be released incrementally ★ — Splitwise
- workspace section base + capacity, stable under capture ★ — FlashInfer
- indirect (device-side) launch bounds for data-dependent work counts ★ — MoE, speculative decoding
- whether an intermediate is live into a backward pass ★ — FA1, survey

**Arithmetic character** (not one bit ★)
- matrix-core FLOPs, vector-ALU FLOPs, and bytes, **separately** — FA2's 16:1 ratio
- stored bit width of weights and activations (bytes moved ≠ compute width) — survey, §13.1.1
- derived class = `memory | compute | launch-bound`, computed against the **measured** per-device
  `R` and `T_crossover`, never asserted by the author

**Policy / control**
- schedule policy: throughput | latency | frame-budget, with a deadline for the last two
- a **compute-unit budget** for this launch ★ — FlashInfer/Nanoflow SM partition, DistServe interference
- a per-iteration cost budget `τ` — Sarathi
- power class (does this launch push toward TDP?) ★ — Splitwise
- RNG stream identity ★ — Megatron
- phase tag (initial / incremental) and a forward/backward tag — survey, FA2
- a capture/replay identity so a repeated launch sequence submits once ★ — FlashInfer, speculative
  decoding's draft chain

## 13.9 The ten requirements the whole corpus agrees on

1. **When a launch is classified, the class must be derived from `(FLOPs, bytes, T, l_qo, g,
   b_w)` against a measured per-device `R`** — never declared by the kernel author, never per
   operator. The same attention operator spans ~400× in intensity, and the same GEMM flips class at
   `T_crossover`.
2. **When two runnable launches have opposite classes, prefer to co-run them; when they have the
   same class, serialize.** (Splitwise's phase characterization; Sarathi's free-compute argument.)
3. **When two launches are co-run, they must be granted disjoint compute-unit sets and the tile
   plan must be recomputed against the grant** — sharing a batch instead costs 3–6× (DistServe) up
   to 28.3× (Sarathi).
4. **When a launch's work items are non-uniform, it must still be ONE launch**, with a cost-balanced
   assignment of items to compute units (`α·l_q + β·l_kv`). Skewed-length workloads lose **38% of
   ITL** to bad balance alone (FlashInfer).
5. **When any geometry is chosen, it must be a multiple of the kernel's tile granularity.**
   257 vs 256 = **+32%** (Sarathi); vocab padded to 128× (Megatron).
6. **When the implied workgroup count is below the device's CU count, add a reduction axis
   (split-K / KV-chunk) before launching** — FA2's "≥80 on 108 SMs", FlashDecoding, FlashInfer's
   split-K writethrough.
7. **When long-lived state (KV, weights, streaming ring buffers) is required, it must be modelled
   as a resident pool with occupancy and admission**, with gang-structured eviction and a
   recompute-vs-spill decision driven by measured costs.
8. **When work is data-dependent (MoE expert counts, accepted speculative tokens), the launch bound
   must come from a device buffer**, not a host constant or a padded worst case.
9. **When a repeated launch sequence has stable geometry and stable buffer addresses, it must be
   captured once and replayed** — mandatory for MoE decode swarms, speculative draft chains, and
   every streaming speech/vision pipeline.
10. **When any of the above is changed, the effect must be measurable through the seam**: achieved
    bandwidth, achieved fraction of matrix-core peak, workgroups launched, spill/LDS occupancy, and
    the realized per-item balance. Every paper here found its result by measuring one of these, and
    three of them (FA1→FA2, FlashInfer's LB ablation, Switch's capacity factor) found that the
    obvious reasoning was wrong.

---

# Part D — Game rendering pipelines and engineering simulation

# SYNTHESIS A — THE RENDER-GRAPH MODEL AS SCHEDULER REQUIREMENTS

To host a game renderer, a general compute scheduler must be able to express and satisfy the
following. This is the union of Frostbite FrameGraph, Unreal RDG, Unity SRP render graph,
Halcyon, and Arntzen's deep dive, with the vendor async-compute rules layered on.

### A.1 Passes and resources
1. When a unit of work is submitted, it must be a *pass*: a name, a set of resource
   declarations, a queue-class tag (raster / compute / copy / async-eligible), and a deferred
   recording callback. (game)
2. When a pass declares a resource, the declaration must be the binding itself — reads,
   writes and read-writes distinguished — so the declared set cannot drift from the accessed
   set. (game)
3. When a resource is written but never read by a surviving pass, the producing pass must be
   culled. (game)
4. When a resource's declared access is write-only, the runtime must treat the previous
   contents as discardable (`DONT_CARE`), and when read-write, it must preserve them. (game)

### A.2 Derived barriers and transitions
5. When the graph is compiled, every barrier, layout transition and cache flush/invalidate
   must be derived from the declarations; no pass author may write one. (game)
6. When a resource is already in the required state, no barrier may be emitted for it. (game)
7. When latency can be hidden, transitions must be emitted as split barriers (begin early,
   end late). (game)
8. When barriers are emitted, they must be batched, and the total count must be on the order
   of twice the number of written surfaces — a graph that emits materially more must be
   flagged. (game)
9. When resource aliasing is enabled, the runtime must account for the aliasing-barrier and
   discard cost, measured at **~5% of frame time on PC drivers** (Halcyon), and must be able
   to disable aliasing when that cost exceeds the memory saving. (game)

### A.3 Transient resource lifetime and memory
10. When two transient resources have disjoint live ranges, they must be eligible to share
    memory, and the runtime must plan those allocations across the whole execution timeline
    rather than greedily. (game)
11. When a resource's last reader completes, its memory must be reclaimable without an
    explicit free. (game)
12. When a resource must outlive the graph, it must be declarable as external and excluded
    from aliasing. (game)

### A.4 Queue assignment and co-running
13. When a pass is marked async-eligible, the runtime must place its cross-queue wait after
    the last producer and its signal before the first consumer, automatically. (game)
14. When choosing co-runners, the runtime must consult a per-pass bottleneck profile
    (ALU / bandwidth / L1-L2 / RT core / tensor / geometry front-end) and must reject pairs
    that saturate the same unit. Measured effect of getting this right vs wrong:
    **GTAO over rasterized shadows 2.10 ms vs over raytraced shadows 3.22 ms** on RTX 3080
    Mobile. (game)
15. When co-running is enabled, the achievable saving to plan for is **1.1–1.9 ms per frame
    at 1080p on RTX 3080 Mobile**, and **"up to 10%"** frame time per joint AMD/NVIDIA
    guidance — not a multiple. (game)
16. When a co-runner is admitted, the runtime must be able to cap its wave/CU allocation;
    uncapped async compute steals all CUs and thrashes caches (**up to 1.5 ms recovered by
    capping, DOOM on PS4 GCN**). (game)
17. When the same graph is submitted with the same inputs, queue placement must be identical
    frame to frame; heuristic re-placement per frame is a defect because it destroys frame-time
    determinism. (game)
18. When a cross-queue dependency is emitted, its cost must be priced at approximately one
    submission (**"each fence is about the same CPU and GPU cost as ExecuteCommandLists"**),
    and signal granularity must be assumed no finer than one per submission. (game)
19. When the platform exposes one geometry front-end, the runtime must not create more than
    one graphics queue; when vendor guidance says extra compute queues do not pay, the default
    must be one graphics + one compute + one copy. (game)
20. When a barrier forces a wait-for-idle that drains a queue, that drain window must be
    treated as a placement opportunity for independent work. (game)

### A.5 Frame budget and pacing
21. When a frame is scheduled, it must be scheduled against a fixed deadline —
    **16.67 ms at 60 Hz**, with mobile-class targets of **8–10 ms CPU + 8–10 ms GPU and
    2–3 ms reserved headroom** — and CPU and GPU time must be reported separately. (game)
22. When a frame finishes early, it must not be presented early. (game)
23. When frames run long, the runtime must inject a wait rather than let submissions queue,
    because queueing adds a permanent frame of latency (buffer stuffing). (game)
24. When the frame rate cannot be held, the runtime must step down the display's refresh-rate
    ladder (e.g. 90 → 60 → 45 → 30 on a 90 Hz panel) rather than free-run. (game)
25. When a subsystem is given a per-frame budget, it must be able to shed work (LOD) to stay
    inside it — the mechanism that keeps a **13–40 ms** procedural workload inside a
    frame. (game)
26. When work has no dependency on the current frame, it must be eligible to start during the
    current frame — DOOM overlaps frame N's post-processing with frame N+1's shadow/depth/
    opaque, and that cross-frame window is where the async gain actually lives. (game)

### A.6 Construction cost and concurrency
27. When the graph is rebuilt per frame, construction must be a serial pass (so chained
    passes can size their outputs from their inputs) and evaluation/recording must be
    parallel. (game)
28. When recording, commands must be stateless so recording parallelizes; redundant-state
    filtering happens once, serially, at lowering. (game)
29. When a pass needs a resource produced far upstream, it must be findable through a typed
    scope lookup rather than an explicitly authored edge, or the graph acquires
    O(passes²) coupling. (game)
30. When the graph supports composition, sub-graphs must be able to run at different
    frequencies (every frame, every N frames, on another queue, on another device). (game)

---

# SYNTHESIS B — THE SIMULATION-STEP MODEL AS SCHEDULER REQUIREMENTS

A simulation step is a DAG of dependent kernels executed thousands to millions of times with
identical topology and changing data. The dominant cost at strong scale is not compute — it is
submission, synchronization, and queue starvation.

### B.1 The launch-overhead budget (the numbers to design against)
1. When per-kernel launch cost is budgeted, use **2–10 µs per kernel launch or memcpy** and
   **<1 µs per event/dependency API call** (GROMACS on H100-class hardware). (simulation)
2. When estimating how much of a step is overhead, note a real MD step issues
   **~20 launch calls + ~30 event calls**, which at peak iteration rate is
   **over 50% of host wall-time** and leaves idle gaps between kernels. (simulation)
3. When the naive per-kernel path is measured, budget **9.6 µs/kernel with a sync per kernel,
   3.8 µs overlapped, 3.4 µs via graph, against a 2.9 µs execution floor** (NVIDIA
   microbenchmark). (simulation)
4. When the scheduler's own per-node host cost is budgeted, it must stay under **~1 µs**;
   Ginkgo's entire solver framework dispatch is **1.00–1.51 µs per iteration**, and a
   scheduler costing more than that has eaten what it saves. (simulation)
5. When the runtime is benchmarked, a degenerate (1-element) problem must be in the suite so
   framework overhead is measured directly rather than hidden behind kernel time. (simulation)

### B.2 Graph capture and replay
6. When a DAG's topology repeats, the runtime must capture it once and replay it, provided it
   will be replayed at least **~3 times** (the measured crossover). (simulation)
7. When batching iterations into one graph, the batch must be capped at a measured optimum —
   **~50–100 kernel nodes on A100-class hardware**, essentially workload-independent — and
   not at the whole loop. (simulation)
8. When graph node count approaches the platform's linear-creation regime (**~2500 nodes on
   A100**, above which creation time and execution time both degrade), the runtime must split
   into multiple graphs. (simulation)
9. When sizing a graph, graph creation must be priced at **~4.2 µs per node** plus a
   0.16–0.42 ms base on A100, and graph memory must be priced as linear in node count. (simulation)
10. When a captured graph's parameters change but its topology does not, the runtime must
    update in place rather than re-instantiate (**~400 µs** to instantiate). (simulation)
11. When reporting expected benefit, the honest range for whole-application capture on real
    solvers is **2–12%** (NAS CG +3.3% avg, LU +7% avg on A100, max +11.87%), and it can be
    **negative** for short runs — so capture must be a measured per-graph decision with a
    live fallback. (simulation)
12. When the backend is HIP, the same capture/replay path must exist (`hipGraph`). (simulation)

### B.3 Queue starvation and pipelining
13. When ready work exists, the device queue must never be empty; **queue-empty time must be a
    first-class measured metric** — "one of the most challenging problems currently faced by
    large-scale distributed-memory GPU-based computing is preventing this queue from becoming
    empty" (PETSc). (simulation)
14. When a foreign API call requires a drained queue (GPU-aware MPI does, on every call), the
    runtime must classify it as a full barrier, count it, and schedule around it. (simulation)
15. When the step is fully device-resident, the runtime must allow **tens to hundreds of steps**
    to be enqueued before requiring a host synchronization. (simulation)
16. When graph analysis is performed, it must run concurrently with device execution on a
    dedicated thread, connected by single-producer-single-consumer queues, so the runtime's own
    analysis is never on the critical path. (simulation)
17. When submission to the backend has latency, a separate backend thread must absorb it. (simulation)

### B.4 Halo / boundary overlap
18. When a kernel's inputs divide into an independent (interior) part and a
    transfer-dependent (boundary) part, the runtime must support splitting it so interior
    compute runs during the exchange, without the caller hand-writing the overlap. (simulation)
19. When a kernel produces a value another rank or device is waiting on, it must be markable
    so it is dispatched ahead of same-stage local work — GROMACS had a *hardware stream
    priority bit added* for exactly this, and notes only one bit exists. (simulation)
20. When priorities are offered, there must be **at least three tiers**: GROMACS gained
    **up to 10%** by moving pruning to a low-priority stream and adding a medium tier for
    reduction/update so it preempts pruning while staying below the critical path. (simulation)
21. When overdecomposition increases the launch count past the point where overlap pays, the
    runtime must fuse and/or capture to compensate — the two must be usable together. (simulation)
22. When a communication step is expressible as a device-side kernel, it must be legal inside
    a captured graph rather than forcing a host round-trip. (simulation)

### B.5 DAG derivation from access sets
23. When tasks declare data handles with access modes, the runtime must derive the DAG from
    those alone, and the parallel execution must be equivalent to sequential insertion order. (simulation)
24. When two tasks both hold read-only access, no edge may be generated; when a buffer is
    declared *reduce*, concurrent producers must not be serialized against each other. (simulation)
25. When mapping the DAG onto queues, the runtime must levelize it, assign queues round-robin
    within a level bounded by the platform's max kernel concurrency (**e.g. 32**), and emit
    no event for a dependency whose endpoints share a queue. (simulation)
26. When kernels in one concurrency group have unequal loads, the runtime must scale their
    launch geometry so their durations match, and must segment any kernel too large for the
    group's spare capacity — worth **up to 32.8% worst-case makespan / 21.3% measured**. (simulation)
27. When group ordering matters, it must be enforced with explicit dependency edges, never
    with stream priority — "kernel-level preemption through prioritized CUDA streams is
    unreliable", and the hardware scheduler's execution order for concurrently launched
    kernels is non-deterministic. (simulation)
28. When a loop inserts tasks faster than they retire, the resident graph must be bounded by a
    window and periodically compacted by horizons that prune completed regions. (simulation)
29. When buffer extents are not known until later tasks are seen, the runtime must look ahead a
    bounded window before committing a backing allocation, to avoid alloc/copy/free resize
    chains. (simulation)

### B.6 Placement, backends and preemption
30. When a kernel has multiple backend implementations, the scheduler chooses the backend; when
    it places a kernel, it must price the implied data movement, and must not re-transfer a
    buffer that already has a valid replica on the target device. (simulation)
31. When a scheduling policy is chosen, it must be replaceable without changing kernel code,
    and any performance model must be built from measured history with graceful cold-start. (simulation)
32. When a deadline is tighter than the platform's measured preemption cost, the runtime must
    use admission control instead of preemption. Budget: A100 context is **44.3 MB**
    (108 SM × 420 KB), **40 µs** to save at 1.1 TB/s, **~100 µs** measured end-to-end, and a
    200 µs timeslice yields in **≤160 µs**. Block-granularity preemption can *add*
    **66.4% (5 ms)** to a latency-critical task's latency. (simulation)
33. When a kernel is not idempotent — the normal case in simulation, since global state changes
    during execution — kill-and-restart must never be used as the preemption mechanism. (simulation)
34. When per-item work is small relative to launch cost, the runtime must be able to lower a
    kernel DAG onto a persistent worker loop with software queues instead of discrete
    launches (**up to 4× over successive launches**, Whippletree), using block-local shared
    memory queues where producer and consumer are the same block (**up to 2×** further), and
    falling back to a global queue when load balance dominates. (both)

---

# Part E — Deadline-driven pipelines (robotics-edge corpus)

# Synthesis for deadline-driven pipelines

## S.1 A multi-rate DAG model a runtime could implement

### S.1.1 Entities
```
Stream   σ_i = (T_i, D_i, φ_i, class_i, policy_i)
             T_i     period            (frame period / control period / solver step)
             D_i     relative deadline (D_i ≤ T_i; implicit deadlines are the common case)  [P: DARIS]
             φ_i     phase / offset
             class_i LATENCY_CRITICAL | BEST_EFFORT                                          [P: DARIS HP/LP]
             policy_i THROUGHPUT | LATENCY | FRAME_BUDGET(b) | ENERGY(j)                      [X]

Node     v ∈ V_i, with:
             cost[backend]        measured MRET vector, per backend, per representation       [P: DARIS + cuRobo Tbl 5]
             character            COMPUTE_BOUND | MEMORY_BOUND | LATENCY_BOUND | BARRIER      [X, motivated by MegBA/offload]
             reads[], writes[]    buffer sets                                                  [cajeta's existing seam]
             transition_in/out    cost to enter/leave this backend                             [P: survey 1]
             footprint            device memory, incl. fixed index overhead                    [P: mmap 4·N³]
             capacity_limit       structural caps (cuRobo's 1024 spheres)                      [P]

Edge     (u → v) intra-job precedence.
```

### S.1.2 Multi-rate edges (the part none of the papers formalizes) **[X]**
Cross-stream edges connect graphs of different periods. Three kinds, all needed:
- **Sample edge** (fast consumer, slow producer, `T_prod > T_cons`): consumer
  reads the most recent *complete* producer output through a double buffer.
  Never blocks. This is OpenVLA's action chunking generalized: one producer job
  serves `N = ceil(T_prod / T_cons)` consumer periods, and `N` must be declared
  so the runtime can size the buffer and detect the failure mode.
- **Accumulate edge** (slow consumer, fast producer): consumer reads a reduction
  over the producer jobs since its last release. Needs a reduction kernel that is
  itself a node.
- **Rendezvous edge** (`T_prod == T_cons`, harmonic): ordinary precedence.

Restrict periods to a harmonic set where possible; the hyperperiod is then
`max(T_i)` rather than `lcm(T_i)`, which is what makes the admission test in S.2
cheap. Non-harmonic rates are permitted but the runtime should say so, because
the test then runs over a bounded prefix instead of an exact hyperperiod.

### S.1.3 Release and deadline derivation
```
job k of σ_i:  r_i,k = φ_i + k·T_i
               d_i,k = r_i,k + D_i − fixed_downstream_latency_i     [P: cuRobo 58/68 ms]

node v in job k, virtual deadline (DARIS's proportional rule):      [P]
               d_v = r_i,k + (Σ_{u ⪯ v} mret_u / Σ_{u ∈ V_i} mret_u) · D_i_effective
```
`fixed_downstream_latency` is required, not optional: cuRobo measured 58 ms
(UR5e) and 68 ms (UR10) between "trajectory sent" and "robot moves". The
schedulable budget is never the whole period. The game analogue is
present-to-photon latency; the HIL-simulation analogue is DAC/actuator latency. **[X]**

## S.2 An admission test cheap enough to run online

Two tiers, because the corpus shows the expensive one cannot be online.

**Tier 1 — per-arrival, O(#partitions), a handful of adds (adopt DARIS verbatim):** **[P]**
```
u_new = mret_new / T_new
for each partition k:
    U_k^remaining = N_lanes_k − U_k^latency_critical
    if U_k^besteffort_active + u_new < U_k^remaining:  ADMIT into k
migrate to partition with earliest predicted finish time, else REJECT
```
Cost is a few nanoseconds. It is a *utilization* test, so it is necessary-but-not-
sufficient for hard deadlines — which is exactly right for the classes cajeta
targets, and matches DARIS's measured outcome (zero HP misses, LP DMR <2%).

**Tier 2 — per-admission-of-a-new-stream, demand-bound function (adopt XAuto's constraints (4)(5)):** **[P] with an [X] bound**
```
dbf_i(t) = max(0, floor((t − D_i)/T_i) + 1) · C_i
test:   ∀ t ∈ TestSet:   Σ_i dbf_i(t) ≤ t · m_lanes
```
`TestSet` = the deadline instants inside the hyperperiod. **[X]** With harmonic
periods that is `Σ_i (H/T_i)` points where `H = max(T_i)`; for a 16.67 ms frame
with sub-rates at 2x/4x/8x this is tens of points, evaluable in microseconds.
For non-harmonic periods, truncate to the first busy-period prefix (the standard
bound) and mark the result as *sufficient-with-a-horizon*, not exact.

**Tier 3 — offline only.** XAuto's full ILP: ≤20 nodes in <1 min with Gurobi on
32 threads; brute force is >24 h at 20 nodes. AxoNN's Z3: 5 s for one transition,
~1 min for three. D-HaX-CoNN re-solves in <2 s for two DNNs at <2% CPU with a
10 s re-plan interval. **[P]** So: never per-frame; at most per-10-seconds; and
only for problems under ~20 schedulable nodes. Above that, use the sorted
finish-time heuristic.

**Empirical fallback.** When cost models are unavailable, survey 1's LP
scheduler used a *feasibility-ratio vs utilization-ratio* curve, measured. At
utilization 1 it sustained ~12 concurrent DNN/RNN instances, and adding
DLA+GPU+CPU pushed 100% feasibility to 2.8x the DLA-only capacity before
collapsing past 3.4x. **[P]** A runtime can build the same curve per platform at
install time.

## S.3 Priority inheritance across kernel chains

None of the ten papers implements classical priority inheritance. Two mechanisms
in the corpus do the equivalent job, and both are cheap: **[P]**

1. **Terminal-node promotion (DARIS "Last").** The final node of a chain is
   dispatched one level above its class. Removing it cost **+38% worst-case
   high-priority response**. In a frame graph this is the present/composite pass;
   in a solver step it is the state write-back. **[P]**
2. **Missed-predecessor promotion (DARIS "Prior").** A node whose predecessor
   missed its virtual deadline is promoted one level. This propagates urgency
   *forward along the chain* as lateness accumulates — a chain that is falling
   behind climbs the priority ladder. Removing it raised average response for all
   tasks. **[P]**

**What genuine inheritance requires, and what cajeta already has.** **[X]** True
priority inheritance is needed when a low-priority kernel *holds a resource* a
high-priority chain needs. On a GPU the resources are (a) buffers and (b) the
partition itself. cajeta's submit seam already carries **read/write buffer
sets**, which is precisely the resource declaration:
```
when a BEST_EFFORT node B writes a buffer that a LATENCY_CRITICAL node L reads,
    and L's chain is ready,
    B inherits L's priority until B completes.
```
This is the WAR/RAW hazard the seam already computes, reinterpreted as a priority
ceiling. It costs one max() at dispatch. Combined with the two DARIS promotions
it gives: forward urgency propagation (missed predecessor), chain-tail protection
(last node), and hazard-based inheritance (buffer overlap).

**The bound on all of it.** Inheritance only helps if the holder can be finished
or preempted quickly. XAuto measured **275 µs** to preempt and switch on Orin's
GPU. **[P]** So the rule is: inherit and let it run to completion if the holder's
remaining time is under ~275 µs; preempt only above that.

## S.4 Where preemption is actually available on consumer GPUs, and at what granularity

**Paper-sourced:** **[P]**
- Jetson Orin GPU, via TSG (Time Slice Group) enable/disable on the CUDA context
  handle: **275 µs to preempt + switch.** XAuto.
- Jetson Orin DLA, via cuDLA command-queue suspend/resume: **132 µs.** XAuto.
- DARIS's "staging" — synchronization points between DNN sub-tasks — is described
  explicitly as the substitute for hardware preemption: *"a synchronization-based,
  coarse-grained preemption mechanism."* Removing it cost **33% throughput** and
  **+22.5% low-priority deadline misses.**
- Survey 1's 13 schedulers: **none of them preempt.** The entire Jetson
  scheduling literature it reviews is placement + pipelining.

**My assessment of the current landscape, not from the corpus:** **[X]**
| Backend | Mechanism actually reachable from a userspace runtime | Granularity |
|---|---|---|
| NVIDIA consumer (Pascal → Ada) | Stream priorities (`cudaStreamCreateWithPriority`, range from `cudaDeviceGetStreamPriorityRange`, typically 2–3 levels); **green contexts** (`cuGreenCtxCreate`, CUDA 12.4+) for in-process SM partitioning; MPS `CUDA_MPS_ACTIVE_THREAD_PERCENTAGE` for cross-process; CUDA Graphs | **Kernel boundary.** Hardware compute preemption exists on Pascal+ but is used by the driver for the display watchdog and the debugger; there is no application API. |
| NVIDIA datacenter | above, plus MIG (A100/H100/A30) | Hard partition, static |
| AMD (RDNA / CDNA, ROCm) | `hipStreamCreateWithPriority`; **CU masking** (`hipExtStreamCreateWithCUMask`) — a per-stream compute-unit bitmask, the closest analogue to green contexts and available on consumer parts; AQL barrier bits for in-queue dependencies without a host round-trip; MES / CWSR (compute wave save-restore) exists in the kernel driver for queue timeouts but is not application-visible; SPX/CPX partitioning on MI300 | **Kernel boundary** for applications; CU masking gives spatial partitioning at CU granularity |
| Vulkan / SPIR-V | Queue priorities (advisory float at device creation); multiple queue families (graphics / compute / transfer); `VK_EXT_global_priority` (LOW/MEDIUM/HIGH/REALTIME — actually enforced by some drivers, typically gated on privilege); timeline semaphores | **Kernel/dispatch boundary.** No preemption API. |
| CPU fallback | OS thread priorities, real cooperative yield | Instruction |

**The conclusion this forces for cajeta.** On every GPU backend cajeta targets,
preemption is a **kernel-boundary** property. Making preemption finer therefore
means making kernels shorter — which reintroduces the launch overhead cuRobo
measured at 10x. The only way out of that trade is a **cooperative tile kernel
that polls a yield flag between tiles**: a long-running kernel that is
*software*-preemptible at tile granularity, so it gets graph-capture economics
and preemption granularity at the same time. That is DARIS's staging pushed one
level down, from between-kernels to between-tiles, and it is exactly what the
"cooperative tile" surface is for. **[X]**

Practical granularity target: a tile whose duration is ~10–50 µs. Below that the
yield check costs too much relative to work; above that the preemption latency
exceeds the 275 µs figure that makes preemption worth doing at all. **[X]**

## S.5 Energy and power as a schedule input

**Paper-sourced:** **[P]**
- Three of the 13 schedulers in survey 1 (AxoNN, Map-and-Conquer, MaGNAS) use
  energy as a **hard constraint**, not a weighted term: `min latency s.t.
  energy < ECT`, and Map-and-Conquer adds `latency < LT` and `size(F,I) < M`.
  Map-and-Conquer achieved **630 mJ → 136 mJ (4.62x)** on VGG-19 at comparable
  25 ms latency.
- **~50.3% of AI-workload energy goes to off-chip traffic** — so *ordering*
  changes energy at fixed FLOPs.
- Precision is the steepest energy lever measured: **0.22 pJ/MAC at 3-bit vs
  1.76 pJ/MAC at 8-bit** (8x), with the mixed-signal advantage collapsing from
  81% to 31% over the same range.
- Platform power for the manipulation stack: VLMaps ~63 W (Thor) / ~50 W (Orin),
  GraphEQA 32/25 W, π0.5 35/30 W, RTAB+nvblox 25/16 W, against a **216 Wh**
  battery with a 30 W baseline draw. Offloading one stage bought **+160%**
  autonomy.
- Jetson power modes (15 W vs 60 W MAXN) change cuRobo's 20-kernel total from
  **316 µs to 662 µs** — a 2.1x cost-model shift from a runtime-settable mode.

**Design consequence.** **[X]** Energy belongs in the policy enum as a *budget
per period* (`ENERGY(joulesPerFrame)`), evaluated as a constraint alongside the
deadline, with three inputs the runtime must own: a measured pJ/op table indexed
by precision, a per-node buffer-traffic count, and a re-derivation trigger on
power-mode change. The last is not optional: a cost table captured at 60 W is
wrong by 2.1x at 15 W.

## S.6 Numeric budgets worth pinning

| Quantity | Value | Source |
|---|---|---|
| **GPU preempt + context switch** | **275 µs** (Jetson Orin, TSG disable/enable) | **[P]** XAuto |
| **Fixed-function accel. queue suspend/resume** | **132 µs** (Orin DLA, cuDLA) | **[P]** XAuto |
| **Individual kernel duration, real pipeline** | **1–12 µs** (RTX 4090), **3–208 µs** (Orin 15 W) | **[P]** cuRobo Tbl 3 |
| **Whole 20-kernel solver iteration** | **46–66 µs** (4090), 94–316 µs (Orin MAXN), 121–662 µs (Orin 15 W) | **[P]** cuRobo Tbl 3/4 |
| **Value of removing launch overhead** | **10x** (CUDA Graph capture of 25 iterations) | **[P]** cuRobo |
| **Implied per-launch host overhead** | ~10–20 µs in a PyTorch/Python path (46 µs device time × ~10x = ~460 µs wall for 20 launches) | **[X]** derived from cuRobo's 10x |
| **Raw launch overhead, C-level** | ~5–10 µs host CPU, ~2–3 µs device gap; graph-replayed node ~1–2 µs | **[X]** my figures |
| **Minimum useful independently-scheduled kernel** | **≥ 100 µs.** Below that, XAuto's scheduler overhead is ~10% at 0.1 ms and the decision costs more than it saves | **[P]** XAuto Fig 12 |
| **Minimum kernel worth existing at all (inside a graph)** | ~1–2 µs; cuRobo ships several 1 µs kernels, but only inside captured graphs | **[P]** cuRobo |
| **Cooperative tile duration target** | **10–50 µs** — long enough to amortize the yield check, short enough that preemption latency stays under the 275 µs switch cost | **[X]** |
| **Host-side overhead ceiling** | **≤ 15%** of device time (cuRobo measured 15/15/12%) | **[P]** cuRobo |
| **Scheduler CPU budget** | **< 0.5%** total for 40 nodes; **< 2%** for an online SMT re-solve every 10 s | **[P]** XAuto, D-HaX-CoNN |
| **Online planner solve budget** | ≤ 2 s for 2 streams; ≤ 1 min for ≤ 20 nodes; re-plan interval ≥ 10 s | **[P]** D-HaX-CoNN, XAuto, AxoNN |
| **Co-run penalty, time-slicing** | **+75%** to the latency-critical task | **[P]** offload Fig 16 |
| **Co-run penalty, unpartitioned spatial** | **+230%** — *worse than serializing* | **[P]** offload Fig 16 |
| **Co-run penalty, 90/10 reservation** | **+3%** latency-critical, **+261%** best-effort | **[P]** offload Fig 16 |
| **Co-run penalty, 50/50 reservation** | **+46%** latency-critical, **0%** best-effort | **[P]** offload Fig 16 |
| **Utilization sampling window** | **≤ 10 ms** — peaks last only tens of ms | **[P]** offload |
| **MRET history window** | **5 samples** (smaller raises DMR, larger costs throughput) | **[P]** DARIS |
| **Latency-critical load cap** | **50% of full load**, above which deadline misses grow exponentially without admission control | **[P]** DARIS |
| **Jitter cost, robotics** | 10 ms mean + 15 ms std added latency = **−10 points** task success; 440 ms vs a 100–200 ms budget = **−50 points** | **[P]** offload |
| **Rate-miss cost, ML** | 1.2 Hz against a 5 Hz contract = **−13.2 points**, entirely recovered when the rate constraint is removed | **[P]** OpenVLA D.4 |
| **Frame-time jitter target** | p99 ≤ 1.25 × budget; frame-to-frame delta ≤ 2 ms at 60 Hz | **[X]** |
| **Solver step jitter target** | p99 ≤ 1.10 × step budget for HIL; unconstrained for offline | **[X]** |
| **Occupancy headroom probe** | sweep 4 → 48 concurrent units; RTX 4090 cost **+8 ms**, Orin MAXN **+158 ms**, Orin 15 W **+417 ms** | **[P]** cuRobo Fig 29 |
| **Batch-1 GPU/CPU crossover** | GPU loses at batch 1 (2.7 ms vs 0.9 ms); crossover ≈ batch 10 | **[P]** cuRobo |
| **Batching gain, by shape** | autoregressive decode 1.6–3.6x; vision encoders 1.0–1.16x; UNet-style 1.08x | **[P]** offload, DARIS |
| **Power-mode cost-model shift** | **2.1x** (Orin 60 W → 15 W, same 20 kernels) | **[P]** cuRobo Tbl 3 |

## S.7 Mapping onto game frame graphs **[X unless marked]**

A render graph at 60 Hz has a **16.67 ms** budget (8.33 ms at 120 Hz) holding 50–300
passes, many of them 20–200 µs. That is cuRobo's regime — 1–12 µs kernels, ~20 per
solve **[P]** — scaled up by an order of magnitude in count. Five direct transfers:

1. **Record once, replay per frame.** cuRobo's 10x from graph capture **[P]** is
   the same result console engines get from pre-recorded command buffers. A
   submit seam that issues per-pass launches will lose an order of magnitude on
   a pass-dense frame. The frame graph *is* the captured graph.

2. **The async compute queue is DARIS's LP class.** DARIS's measured contract —
   zero high-priority deadline misses, best-effort DMR <2%, high-priority
   response 2.5x faster **[P]** — is exactly the contract an engine wants from
   async compute. And DARIS's staging ablation (−33% throughput, +22.5%
   best-effort misses when staging is removed **[P]**) says the async work must
   be cut into yieldable chunks or it will block the graphics queue.

3. **The offload paper's MPS table is a measurement of async-compute starvation.**
   **[P]** Unthrottled co-run costs the latency-critical task **+230%**; an
   explicit reservation brings it to **+3%**. That is precisely the "async
   compute starved my graphics queue" failure console developers describe, with
   numbers attached. On AMD, `hipExtStreamCreateWithCUMask` is the reservation;
   on NVIDIA, green contexts. **The reservation ratio is the frame-budget policy's
   principal knob**, and the 90/10 vs 50/50 asymmetry (3%/261% vs 46%/0%) says
   the right default is nearer 50/50 than 90/10 unless the latency-critical
   stream is genuinely at its deadline.

4. **Present-pass promotion.** DARIS's "Last" rule (+38% worst-case response when
   removed **[P]**) maps onto the final composite/present pass: a missed present
   is a dropped frame no matter how early the G-buffer finished. Promote the
   tail of the chain.

5. **Frame-budget policy = XAuto's objective over one period.** XAuto minimizes
   makespan subject to demand-bound feasibility **[P]**; a frame-budget policy is
   that with the makespan bound fixed at the frame period and everything that
   does not fit deferred to the best-effort class. XAuto's p99 42.0 → 26.1 ms
   result **[P]** is a frame-time result in all but name, and the control that
   matters is that even the *best* static assignment left 37.7 ms — runtime
   ordering carried 1.29x of the win on its own.

Where the mapping breaks: games have a hard *periodic* deadline with no value
after it (a late frame is a dropped frame), whereas the robotics papers mostly
measure soft degradation. So a frame-budget policy should support **discard**, a
mode none of the papers implements: if a best-effort node cannot finish before
present, cancel it rather than let it run into the next frame.

## S.8 Mapping onto time-stepped simulation **[X unless marked]**

A time-stepped solver runs many dependent kernels per step, often with global
reductions, and — for interactive or hardware-in-the-loop use — a wall-clock
budget per step. Four transfers:

1. **MegBA is the shape.** **[P]** Its PCG inner loop is SpMV → dot/axpy → two
   all-reduces, repeated. Every kernel is memory-bound, so co-running two
   solver streams gains nothing; the useful co-runner is compute-bound work.
   That is the whole justification for the arithmetic-character tag on the submit
   seam: **the tag is what tells the scheduler that two ready kernels are
   *not* complementary.**

2. **Barriers are the opportunity.** **[P for the structure, [X] for the reading]**
   An all-reduce drains occupancy. Declaring a node as `BARRIER` lets the
   scheduler pre-stage a complementary kernel into its shadow — the only reliable
   co-scheduling window in a serial solver chain.

3. **Occupancy headroom decides whether co-running is viable at all.** cuRobo's
   Fig 29 **[P]**: 4 → 48 concurrent units costs **+8 ms** on an RTX 4090 and
   **+417 ms** on an Orin 15 W. On a large GPU a solver leaves room; on a small
   one it does not. This must be *probed* at startup per device, not assumed —
   and it is a two-point measurement, so it is cheap.

4. **mmap's three co-equal stages are the multi-rate structure.** **[P]** Its
   acquisition / registration / fusion bands are all comparable and all grow with
   resolution. A simulation has the same shape: a fast inner integrator, a slower
   contact or collision update, a slower render/telemetry path. Those are three
   different periods over shared buffers — the exact case S.1.2's sample and
   accumulate edges exist for. And because no stage dominates, optimizing one in
   isolation is bounded by its share of the step.

**The HIL budget.** For hardware-in-the-loop, the schedulable budget is the
wall-clock step **minus fixed I/O latency** — cuRobo measured 58 ms and 68 ms of
actuation delay on two UR arms **[P]**, which on a 100 ms budget is more than
half of it. A simulation runtime that schedules against the nominal step and
ignores the I/O leg will overrun every time. Make `fixed_downstream_latency` a
required input, not an optional hint.

**What simulation does *not* inherit.** Offline (non-interactive) simulation has
no deadline at all, so the DARIS/XAuto machinery is dead weight there — a pure
throughput policy with graph capture and aggressive fusion is the whole answer.
The deadline apparatus should be inert unless a period is declared.

## S.9 The three findings that most constrain cajeta's design

1. **Preemption is not available at the granularity the problem needs, on any
   consumer GPU, from any of the backends cajeta targets.** The corpus's only
   real preemption number is 275 µs **[P]**, on a Jetson, via a mechanism
   (TSG-on-CUDA-context) that is neither portable nor exposed on desktop parts.
   Survey 1's entire 13-scheduler literature preempts *nothing*. The workable
   answer is the cooperative tile with a software yield check, which is DARIS's
   staging one level down. Design the seam around that, and treat any hardware
   preemption that turns out to be reachable as a bonus.

2. **Unpartitioned co-running is the worst option, not the best.** +230% versus
   +75% for plain serialization **[P]**. A "co-run complementary kernels"
   scheduler that does not also enforce a compute-unit reservation will make
   latency-critical work more than three times slower than doing nothing. The
   reservation — CU masks on AMD, green contexts on NVIDIA — is not an
   optimization on top of co-running; it is a precondition for it.

3. **Per-kernel scheduling decisions cannot be afforded at real kernel sizes.**
   Real kernels are 1–12 µs **[P]**; scheduler overhead is ~10% at 100 µs nodes
   **[P]**; graph capture is worth 10x **[P]**. So the scheduling unit must be a
   *captured chain*, not a kernel, and the scheduler must run at chain-admission
   and chain-completion boundaries — not at every launch. This is the single
   strongest argument for the submit seam carrying whole geometry + buffer-set +
   character + policy for a chain rather than per-dispatch.
