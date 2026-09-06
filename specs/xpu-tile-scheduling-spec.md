# Spec: Tile scheduling — the single-process kernel scheduler over the submit seam (`xpu-tile-scheduling`)

**active** — filed 2026-09-06, approved 2026-09-06 (every open question resolved with the developer; plan in `agents/`). Supersedes
[`xpu-kernel-scheduling`](archive/xpu-kernel-scheduling-spec.md) (approved
2026-08-20, never started), and absorbs the scheduling halves of
[`llm-kernel-scheduling`](archive/llm-kernel-scheduling-spec.md) and
[`robotics-kernel-scheduling`](archive/robotics-kernel-scheduling-spec.md).
Companion documents: [`xpu-tile-manifest`](xpu-tile-manifest-spec.md) (what the
compiler records per kernel) and
[`xpu-tile-workload-profiles`](xpu-tile-workload-profiles-spec.md) (the four
client profiles this scheduler must serve). Research record with every number's
source: [`xpu-tile-scheduling-findings`](xpu-tile-scheduling-findings.md).

## 1. Definition

### 1.1 Purpose

A **session-level, single-process kernel scheduler** in the `cajeta.xpu`
runtime. Given the stream of Tile submissions a program makes during its life,
it decides *when* each runs, *alongside what*, on *which queue and compute-unit
set*, and *whether it is captured for replay*, so that no single kernel's
footprint leaves the device half idle and no deadline-carrying submission is
harmed by best-effort work. It is the consumer of the seams the cooperative-tile
arc reserved (`KernelSubmission`, `Scheduler.submit`,
`KernelResourceDescriptor`, `SchedulePolicy`, `ArithmeticCharacter`) and of the
manifest the compiler now emits.

### 1.2 Why this is a re-write, not an edit

The 2026-08-20 spec was written before the Tile surface existed and before the
corpus was read in full. Five things it assumed are false or obsolete, each
measured in the findings document:

- It made MIG and MPS the top rungs of its partition ladder. Neither exists on
  either reference device, MIG measured *worse* than soft rate control (TGS,
  77 % of exclusive), and both are superseded in-process by HIP CU masks and
  CUDA 12.4 green contexts at microsecond repartition cost.
- It named preemption a tier. Preemption is a 100 µs-class operation on
  current hardware (GPreempt, A100), no userspace interface exists on NVPTX or
  Vulkan, and REEF's 35 µs figure required a modified driver. Deadlines tighter
  than that come from admission, not from stopping.
- It let the author declare the roofline class. The same attention kernel
  spans ~400× in arithmetic intensity between prefill and decode (FlashInfer),
  and a weight-stationary GEMM flips class at a token count that quantization
  moves by 4×. The class must be derived per submission from the manifest.
- It had no notion of graph capture, ragged launches, resident pools,
  accumulating writes, or a frame's forbidden zone — the four things the game
  and simulation corpora say a scheduler cannot host those workloads without.
- It scheduled per kernel. Real kernels in real pipelines run 1–12 µs, a
  scheduling decision costs about 10 % of a 100 µs node, and capturing a
  20-kernel chain was worth 10× (cuRobo); the unit of scheduling must be the
  chain.
- It carried a kernel-library gap catalog that is now stale (the scan spec
  exists; cajeta-llm built attention, norms, sampling and MoE routing).

### 1.3 Scope

- **In:** the submission model; per-submission classification against a
  measured device; the dependency graph derived from access sets; admission and
  co-residency; launch-rate control; compute-unit partitioning where the
  backend offers it; the four schedule policies and their deadline semantics;
  graph capture and replay; resident pools as a scheduled resource; the
  feedback loop through the profiler's record sink; the per-device calibration
  set; the backend mechanism ladder.
- **Out:** cluster and multi-node placement; driver modification; a certified
  hard-real-time scheduler; graphics-queue rasterization (the Vulkan backend is
  compute-only; the game profile is scoped to a renderer's compute passes and
  the frame discipline — profiles spec §2); autotuning of kernel internals
  (`xpu-kernel-scheduling-hints`, `kernel-occupancy-autotune`).

### 1.4 Principles

- **Derived, then measured, never declared.** Class, footprint, restartability
  and duration come from the manifest and the device; an author's tag is a
  hint the runtime reports disagreement with.
- **Admission before partition before yield.** The cheapest control that meets
  the requirement wins: do not launch what you would regret; cap what you
  launch; stop only what can be safely stopped.
- **Soft over hard.** Rate control and CU masks, repartitioned at frame or
  step boundaries, beat fixed hardware slices in this regime (TGS vs MISO).
- **Bandwidth is the shared resource nothing partitions.** Every admission
  decision accounts for DRAM and last-level-cache pressure explicitly; no
  consumer part isolates them.
- **The scheduler is off the critical path.** Its per-decision cost is
  budgeted, measured, and reported; a scheduler that costs more than it saves
  is a defect.
- **Results are invariant.** No scheduling decision changes a kernel's output.
  Order-dependent float accumulation is documented and never co-run.
- **Single process is the advantage.** No context-switch tax (MASK: 8–12 % per
  process), no cross-address-space TLB channel, complete buffer sets at submit
  (which is what removed the deadlock that made Salus reject kernel-granularity
  scheduling).

## 2. The submission

- **2.1** When a kernel is launched, the launch site hands the runtime a
  **value-typed submission**: the kernel, its arguments, geometry (grid and
  block, or a ragged item list), the target stream or queue class, and the
  policy fields below. It is never a closure over the launch site (Paella),
  which also sidesteps the open kernel-launch-in-lambda defect. **Every launch
  is a submission** (developer, 2026-09-06): the existing
  `k.launch(s, grid: …, block: …)(args)` form is a submission with the kernel's
  default policy and access sets from the manifest, so no existing call site
  changes; a `KernelSubmission` passed in place of the stream adds what a
  default cannot know — policy, deadline window, protection, a compute-unit
  budget, a capture identity, an energy budget.
- **2.1a** When the seam's own cost is measured, it is within measurement
  noise for kernels above 100 µs and reported below that; a runtime switch
  bypasses the scheduler entirely for A/B measurement, and that bypass is the
  only unscheduled path.
- **2.2** When a submission is built, its read set, exclusive-write set,
  accumulate set and indirect-bound set are the manifest's parameter modes
  bound to the actual buffer handles; the author does not restate them.
- **2.3** When a submission names a policy, it is one of `throughput`,
  `latency`, `frameBudget`, or `energy`; `latency` and `frameBudget` carry a
  deadline window `(earliest, latest)` in the host clock; `energy` carries a
  joules-per-period budget on a periodic stream and is evaluated as a
  constraint alongside that stream's deadline (§8.7); a submission with no
  policy takes the kernel's manifest default, else `throughput`.
- **2.4** When a submission is protected (`latency`, `frameBudget`, or an
  explicit protected bit), no scheduling action may defer or throttle it; when
  it is best-effort, every control in §6–§8 may act on it.
- **2.5** When a submission carries a compute-unit budget (a count or a
  fraction), the dispatcher honors it where the backend can (§7) and the tile
  planner re-plans the launch against it (manifest §5.4).
- **2.6** When a submission names a capture identity, the runtime may treat
  repeated submissions with the same identity, stable geometry and stable
  buffer addresses as one replayable graph node (§9).
- **2.7** When a submission references a resident pool (§10), it names the pool
  and the per-item reservation it needs; admission checks the pool before the
  device.
- **2.8** When a submission is made and the manifest for `(kernel, active
  target)` is missing, the launch proceeds as an unclassified submission — no
  co-residency, `throughput` semantics, measured from its first run — with a
  diagnostic; there is no separate direct path (§2.1a).
- **2.9** When a submission is rejected (unmeetable deadline §8.2.3, pool
  exhaustion §10.4, oversubscribed device §5.7), the rejection is returned to
  the caller with a reason; it is never silently dropped or silently run late.

## 3. Classification against the measured device

- **3.1** When a submission is classified, the runtime evaluates the manifest's
  cost expressions over the submission's arguments to obtain matrix-core
  operations, vector operations, and bytes moved, and computes operational
  intensity as bytes of device-memory traffic per operation.
- **3.2** When the device's ridge point is known (measured peak matrix and
  vector throughput over measured achievable bandwidth, §12), the class is
  `computeBound` above it, `memoryBound` below it, and `indeterminate` within a
  factor of two of it (roofline); the class is recomputed per device because
  the ridge point differs.
- **3.3** When a submission's predicted duration is below a configured multiple
  of the backend's measured launch cost (default 5×), it is additionally
  classed `launchBound`, which makes it a capture candidate (§9) and a fusion
  candidate.
- **3.4** When a manifest field is `measured`, the first execution of that
  `(kernel, geometry bucket)` runs solo, its achieved bytes and duration fill
  the field, and only subsequent submissions are eligible for co-residency
  (Orion).
- **3.5** When a submission's class disagrees with the manifest's declared
  hint, the runtime records the disagreement and uses the derived class.
- **3.6** When a kernel's measured duration at fixed geometry varies by more
  than a configured fraction (default 10 %) across a rolling window, it is
  marked **unpredictable** and excluded from duration-based admission; a kernel
  whose window is within the fraction is **predictable** and may be scheduled
  against its high percentile (Clockwork measured branch-free kernels within
  0.03 % of median at the 99.99th percentile; the cooperative surface produces
  that shape of kernel).
- **3.7** When a stored-width field is present (4-bit weights, 8-bit
  activations), bytes moved use the stored width and operations use the
  compute width; the crossover token count is `T = R · b_w / 2` algebraically
  and is *measured* per kernel family because the practical value is 2.5–3×
  the algebraic one (Sarathi, DistServe).

## 4. The dependency graph

- **4.1** When two submissions are both pending, an edge is derived from their
  access sets alone: write-after-read, read-after-write and write-after-write on
  a shared handle order them; read-after-read never does; the parallel
  execution is equivalent to sequential submission order (StarPU, Legion).
- **4.2** When two submissions accumulate into the same buffer, they are
  ordered; v1 does not provide private partials.
- **4.3** When a stage's output is another stage's input in a chain, the buffer
  handoff is a scheduling constraint the dispatcher places, not merely an edge
  it respects (survey task-chain model).
- **4.4** When the graph is mapped onto queues, it is levelized; independent
  submissions within a level are spread across queues round-robin up to the
  backend's measured concurrency, and no event is emitted for an edge whose
  endpoints share a queue.
- **4.5** When ordering between concurrent submissions matters, it is enforced
  with an explicit edge or event, never with stream priority, whose ordering is
  unreliable on every backend studied.
- **4.6** When a foreign call requires a drained device queue (GPU-aware
  interop, a Vulkan wait-idle), the runtime classifies it as a full barrier,
  counts it, and reports the count; queue-empty time is a first-class metric.
- **4.7** When submissions arrive faster than they retire, the resident graph
  is bounded by a window and periodically compacted at horizons that prune
  completed regions (Celerity).

## 5. Admission and co-residency

- **5.1** When two submissions are candidates to run concurrently, they are
  admitted together only if the sum of their per-compute-unit register, shared
  memory and group-slot demands fits the device (manifest §3; occupancy is the
  accounting unit here, never a target).
- **5.2** When both candidates carry the same non-indeterminate class, they are
  not co-run; when the classes differ, or either is `indeterminate`, they may be
  (Orion: 1.08× for a same-class pair at 40 % occupancy each; 1.41× for an
  opposite-class pair).
- **5.3** When a candidate's solo occupancy exceeds a high-water mark (default
  85 %), it is time-interleaved rather than co-resident; when it is below a
  low-water mark (default 15 %), co-residency is preferred (Gandiva's shape,
  re-measured on this hardware).
- **5.4** When a best-effort candidate's read set exceeds a configured fraction
  of the device's last-level cache, it is not co-run with a protected
  submission (MASK's cache channel); a `streaming` access (manifest §6.3)
  is exempt.
- **5.5** When the device shares memory with the host (an APU), the bandwidth
  budget admission works against is the measured achievable bandwidth under
  concurrent host activity, not the idle figure, and transfer terms are zero.
- **5.6** When a protected submission carries a deadline, admission of any
  co-runner uses the three-channel interference estimate (scheduling delay
  linear in resident count, last-level-cache pressure from the co-runners'
  working-set fractions, and a power-droop term where power is readable —
  iGniter) and admits only if the protected submission's predicted latency
  stays inside its window; without a deadline, the class rule and footprint
  rule suffice.
- **5.7** When the device memory needed by a candidate plus resident work
  exceeds capacity, the candidate queues rather than relying on driver paging.
- **5.8** When the interference estimate is uncertain (no window, an
  unpredictable kernel, a power reading absent), the runtime refuses
  co-residency; the accepted price is ≤ 5 % aggregate throughput (gpu-lets).
- **5.9** When a co-residency decision is taken under uncertainty, aggregate
  progress over a bounded window is measured and the decision is reverted if
  progress did not improve; the revert is observable in the feedback records
  (Gandiva).
- **5.10** When a protected submission already fills the device, best-effort
  work is serialized behind it (Clockwork's regime); when it does not, co-run
  (Orion's regime). The footprint vector decides, per submission, never a
  global mode.
- **5.11** When a candidate's implied group count is below the device's
  compute-unit count and its manifest exposes a split axis, the runtime adds
  reduction parallelism before launching rather than run underfilled
  (FlashAttention-2, FlashDecoding).
- **5.12** When a best-effort submission would co-run with a protected one and
  the backend offers no compute-unit reservation (§7), the runtime
  time-interleaves them instead; unpartitioned co-running measured +230 % on
  the protected task against +75 % for plain time-slicing and +3 % with a
  90/10 reservation (manipulation study, Jetson Orin). A reservation is a
  precondition for co-running, not an optimization on top of it.
- **5.13** When a reservation is granted, its ratio is the principal knob: a
  90/10 split cost the protected task 3 % and the best-effort task 261 %; a
  50/50 split cost 46 % and 0 %. The default sits nearer 50/50 and moves toward
  90/10 only when the protected stream's predicted latency approaches its
  window.

## 6. Launch-rate control — the always-on safety net

- **6.1** When a protected submission stream and a best-effort stream are both
  resident, the runtime regulates the best-effort stream's launch rate by
  additive-increase multiplicative-decrease, using the protected stream's own
  iteration or frame period as the feedback signal (TGS, AntMan); no
  driver-reported utilization scalar is ever a control input.
- **6.2** When the protected stream's period deviates from its established
  baseline by more than a threshold, the best-effort rate is set to zero and the
  baseline is re-established before resumption.
- **6.3** When throttling, only best-effort launches are deferred; a protected
  launch is never deferred.
- **6.4** When the sum of predicted durations of already-submitted,
  incomplete best-effort work exceeds a configured fraction of the protected
  work item's budget (default 2.5 %, Orion), no further best-effort submission
  is dispatched until that set drains.
- **6.5** When rate control is active, its cost on a solo-running stream is
  ≤ 5 % throughput, measured across kernels and geometries (TGS: 0.3–5 %).
- **6.6** When power is readable and the resident set's predicted draw exceeds
  the device cap, the clock-droop correction is applied to every duration
  prediction and no submission whose admission drives the predicted clock
  below a floor is admitted; on an APU, host work that shares the package
  budget is part of the demand (iGniter; the findings note this interaction is
  unmeasured and an early experiment).
- **6.7** When the device is in flight, at most N descriptors are outstanding
  to the driver (default 4, REEF); N is configurable and the host cost of the
  chosen N is reported.
- **6.8** When the runtime's view of outstanding work falls below a threshold
  of unstarted groups, the next eligible submission is dispatched even if it is
  not the best choice, so the device never idles on a host round trip (Paella).
- **6.9** When submissions form a chain of short kernels (a solver iteration of
  20 kernels runs 46–66 µs on a 4090, individual kernels 1–12 µs; cuRobo), the
  **scheduling unit is the chain**: admission, classification and placement
  happen at chain admission and chain completion, the chain is captured (§9),
  and no per-launch decision is taken inside it. A kernel is scheduled
  individually only when its predicted duration is at least 100 µs, below
  which the decision costs more than it saves (XAuto: ~10 % overhead at 100 µs
  nodes).

## 7. Compute-unit partitioning — soft, per boundary

- **7.1** When the backend exposes per-stream compute-unit masks (HIP) or
  in-process partitions (CUDA green contexts, 12.4+), a submission's CU budget
  is realized by placing it on a stream or context with that mask; the cost of
  repartitioning is stream or context creation, in microseconds, and it happens
  only at a frame or step boundary.
- **7.2** When a protected submission and a long-running best-effort submission
  are both resident, the protected one receives the *smallest* partition that
  meets its deadline under §5.6, and the remainder goes to best-effort work
  (iGniter's lower bound), never the partition that maximizes its throughput.
- **7.3** When a kernel is first co-run, the runtime measures its scaling knee
  — the smallest partition beyond which throughput per added unit falls below
  a threshold — and never allocates more than the knee while other work is
  queued (gpu-lets `p_eff`).
- **7.4** When a partition assignment is held, no compute unit is left masked
  off while any admitted submission is queued (no-bubble invariant).
- **7.5** When the backend offers no partition (Vulkan, CPU), the ladder
  degrades to rate control (§6) and queue placement (§4.4), never to an error.
- **7.6** When a backend offers a fixed hardware slice whose largest slice is
  smaller than the protected stream's requirement, soft control is preferred
  over the slice (TGS). MIG is not built.
- **7.7** When partition enumeration is needed, it completes in under 1 ms at
  the device's native granularity.

## 8. Policies and deadlines

### 8.1 `throughput`

- **8.1.1** When submissions are ordered under `throughput`, the order is
  shortest predicted remaining work first, bounded by a per-stream deficit
  counter so no stream's deficit exceeds a configured threshold (Paella).
- **8.1.2** When a kernel's fixed launch cost exceeds a configured fraction of
  its per-item cost (the `β/α` ratio of `ℓ(b) = α·b + β`), a bounded delay
  accumulates more items before dispatch; otherwise the delay is zero
  (Clipper: the gain is proportional to `β/α`, and zero for a low-`β` kernel).
- **8.1.3** When a batch geometry is searched, it is by AIMD with a 10 %
  multiplicative decrease against the declared latency target, quantized to
  the manifest's tile granularity, and a geometry change never triggers
  recompilation.
- **8.1.4** When the unbatched submission already saturates the device
  (occupancy above the §5.3 high-water mark), batching is not attempted;
  interleaving is (AlpaServe).
- **8.1.5** When several pending `throughput` submissions name the same kernel,
  their write sets are disjoint, and their combined work axis is below the
  kernel's measured crossover, the runtime coalesces them into one ragged
  launch (manifest §7.1) rather than dispatching them separately; a
  submission whose window would be missed by waiting is dispatched alone. The
  coalesced launch's per-item results are delivered to each original submitter
  (Orca's concatenated launch; Nexus's batching-aware packing).
- **8.1.6** When a coalesced launch is formed, the work axis is the sum of the
  items' work, so a batch that would cross into the compute-bound regime stops
  growing there; batching pays only below the crossover and the runtime knows
  the crossover per kernel family and device (§3.7).

### 8.2 `latency`

- **8.2.1** When a `latency` submission is runnable, new best-effort dispatch
  stops within one scheduler turn, and the observed arrival-to-start on AMDGPU
  is under 100 µs.
- **8.2.2** When a `latency` submission has no opposite-class partner that fits
  under §5, exactly one kernel executes on the device at a time (Clockwork).
- **8.2.3** When a submission's `latest` cannot be met even if dispatched
  immediately, it is rejected at submit with a reason (Clockwork, AlpaServe);
  when `latest` passes while queued, it is dropped and reported, never run
  late.
- **8.2.4** When submissions with materially different predicted durations
  share a queue, they are separated into duration buckets so a short-deadline
  submission never queues behind a long one under first-come order.
- **8.2.5** When a pipeline of stages carries one end-to-end deadline, per-stage
  budgets are derived by optimization over the stage cost curves and the
  inter-stage fan-out, not split evenly (Nexus: an even split cost 19 %).
- **8.2.6** When admission cycles N resident streams with duty cycle `d`, the
  worst-case latency used is `d + ℓ(b)`, not `2·ℓ(b)`.

### 8.3 `frameBudget`

- **8.3.1** When a `frameBudget` stream is declared, it names a period and a
  budget; every submission on it inherits `(earliest, latest)` from the current
  frame, and the frame's CPU and GPU time are reported separately.
- **8.3.2** When the end of a frame's budget approaches, a **forbidden zone**
  opens in which only submissions that provably complete before `latest` are
  admitted; no admitted device work may cross the frame boundary (TimeWall).
- **8.3.3** When submitted work will not complete by `latest`, the frame
  completes with the results available and reports which submissions were cut
  (Clipper's deadline rule), and best-effort submissions are shed **early** —
  those that would force an inefficient geometry, not merely those already
  late (Nexus: early drop beats lazy drop by up to 25 %).
- **8.3.4** When a submission belongs to a degraded-variant family (manifest
  §10) and the frame cannot fit the requested rank, the runtime substitutes a
  lower rank and reports it, in preference to dropping.
- **8.3.5** When the same frame graph is submitted with the same inputs, queue
  and partition placement are identical frame to frame; per-frame heuristic
  re-placement is a defect because it destroys frame-time determinism.
- **8.3.6** When a best-effort co-runner is admitted alongside frame work, its
  compute-unit allocation is capped (§7); uncapped co-runners steal every unit
  and thrash caches (DOOM: 1.5 ms recovered by capping alone).
- **8.3.7** When frame work has no dependency on the current frame, it is
  eligible to start during the current frame; the cross-frame window is where
  async gains live (DOOM overlaps frame N's post with frame N+1's depth).
- **8.3.8** When a frame finishes early, it is not presented early; when
  frames run long, the runtime injects a wait rather than let submissions queue
  (Android frame pacing: queueing adds a permanent frame of latency).
- **8.3.9** When a periodic stream cannot hold its rate, the runtime steps down
  a declared rate ladder and reports it rather than free-running.
- **8.3.10** When a frame-budget stream is resident, the runtime does not
  enter any probing mode (§7.3, §5.9); it uses manifests and prior-frame
  timings only (MISO's probe window is a training-job luxury).
- **8.3.11** When a frame-budget stream co-runs with a throughput stream, the
  frame stream's frame time stays within 10 % of its solo frame time; the
  uncontrolled baseline is expected to exceed 40 % (TGS).

### 8.4 Yielding, not preempting

- **8.4.1** When a protected submission arrives and a yield-capable best-effort
  kernel (manifest §8.1) is running, the runtime sets its yield flag; the kernel
  stops at its next group boundary and its remaining groups are resubmitted.
- **8.4.2** When the running best-effort kernel is not yield-capable, the
  runtime waits for it; the admission rules (§6.4, §8.3.2) exist so that wait
  is bounded by the kernel's predicted duration, which the forbidden zone
  already accounted for.
- **8.4.3** When kill-and-restart is considered, it is used only on a kernel
  the manifest marks restartable, the restart point is at most N in-flight
  descriptors back, and the redundant re-execution is measured and reported
  (REEF); a non-restartable kernel is never killed.
- **8.4.4** When a backend's actual preemption or yield latency is measured, it
  is exposed in the calibration set (§12) and any deadline tighter than it is
  served by admission, not by yield.
- **8.4.5** When a best-effort kernel is admitted, its uninterrupted execution
  is bounded by a configured timeslice realized through group-boundary yields;
  a kernel that cannot yield is admitted only if its predicted duration fits
  the slice.
- **8.4.6** When a yield-capable kernel is shaped, the work between yield
  checks targets 10–50 µs: long enough to amortize the check, short enough
  that the response to a protected arrival stays under the cost a hardware
  context switch would have had (275 µs measured on Orin). Yielding is the
  synchronization-based coarse preemption DARIS calls staging, pushed from
  between kernels to between tiles; removing staging cost DARIS 33 %
  throughput and 22.5 more points of best-effort deadline misses.

### 8.5 Chains, promotion and inheritance

- **8.5.1** When a protected chain's terminal node (the present pass, the
  state write-back) is dispatched, it runs one priority level above its class;
  removing this rule cost DARIS 38 % on worst-case protected response.
- **8.5.2** When a node's predecessor missed its virtual deadline, the node is
  promoted one level, so lateness propagates urgency forward along the chain.
- **8.5.3** When a best-effort submission writes a buffer a ready protected
  chain reads, the writer inherits the reader's priority until it completes; the
  hazard the access sets already compute is the priority ceiling, at the cost
  of one comparison at dispatch.
- **8.5.4** When the inheriting holder's predicted remaining time is under the
  backend's measured yield latency, it runs to completion; only above that is
  it yielded.
- **8.5.5** When virtual deadlines are assigned to nodes inside a chain, each
  node's deadline is the chain's release plus the chain's effective budget
  scaled by the node's share of the chain's measured cost (DARIS's
  proportional rule).

### 8.6 Periodic streams and multi-rate edges

- **8.6.1** When a periodic stream is declared, it names period, relative
  deadline, phase, class and policy, and a **fixed downstream latency** the
  runtime subtracts from the schedulable budget (present-to-photon for a frame,
  actuator delay for hardware-in-the-loop — cuRobo measured 58–68 ms of
  actuation delay on a 100 ms budget); the field is required, not a hint.
- **8.6.2** When streams of different periods share buffers, the edge is one
  of three declared kinds: a **sample** edge (fast consumer reads the most
  recent complete output of a slow producer through a double buffer, with the
  ratio declared so the buffer is sized and starvation is detectable), an
  **accumulate** edge (slow consumer reads a reduction over the producer's jobs
  since its last release, the reduction being a node), or a **rendezvous**
  edge (equal periods, ordinary precedence).
- **8.6.3** When periods are harmonic, the hyperperiod is the longest period
  and the admission test below is exact; when they are not, the test runs over
  a bounded prefix and reports itself as sufficient with a horizon.
- **8.6.4** When a job arrives on an admitted stream, admission is a
  utilization test per partition (new utilization plus active best-effort
  utilization under the partition's remainder after protected load), a few
  additions, a few nanoseconds (DARIS, verbatim); it is necessary, not
  sufficient, which matches the soft classes this scheduler serves.
- **8.6.5** When a new periodic stream is admitted, the demand-bound test over
  the deadline instants of the hyperperiod runs once; for a 60 Hz frame with
  sub-rates at 2×, 4× and 8× that is tens of points, evaluable in microseconds.
- **8.6.6** When an exact placement solver is used at all, it runs offline or
  at intervals of ten seconds or more, on problems under twenty schedulable
  nodes (XAuto, D-HaX-CoNN); never per frame.
- **8.6.7** When protected load on a partition exceeds half its capacity,
  further protected admission is refused; above that point deadline misses
  grow exponentially without admission control (DARIS).
- **8.6.8** When a stream's measured cost history is kept, the window is five
  samples (smaller raised misses, larger cost throughput — DARIS) and the
  utilization sampling window is at most 10 ms, because load peaks last tens
  of milliseconds.

### 8.7 `energy`

Decided in v1 by the developer (2026-09-06). Three of the thirteen edge
schedulers surveyed treat energy as a hard constraint, not a weighted term;
one reached 4.6× lower energy at equal latency (630 → 136 mJ at 25 ms); about
half of an AI workload's energy is off-chip traffic, so ordering changes
energy at fixed work; and precision is the steepest lever measured (0.22 pJ per
multiply-add at 3-bit versus 1.76 at 8-bit).

- **8.7.1** When a periodic stream declares `energy(joulesPerPeriod)`, the
  runtime estimates each submission's energy from the calibration set's
  measured picojoules-per-operation table by precision (§12.7), the manifest's
  bytes moved and stored widths, and the device's measured base power, and
  admits a period's schedule only if the estimate fits the budget alongside
  the stream's deadline; energy is a constraint, never a weighted objective.
- **8.7.2** When the budget cannot be met, the runtime degrades by variant
  family first (precision before rate — profiles spec §6.2), then by the rate
  ladder, and reports each step; it never silently exceeds the budget.
- **8.7.3** When ordering is free of deadline pressure, the runtime prefers
  the order that minimizes off-chip traffic: consumers placed directly after
  their producers, captured chains over discrete launches, streaming loads
  marked non-temporal.
- **8.7.4** When the device's power mode changes, the picojoule table and
  every duration record are re-derived (§12.4b); a table captured at one mode
  is wrong at another.
- **8.7.5** When power is not readable on a device, the `energy` policy
  reports itself unavailable once and behaves as `frameBudget` for that
  stream; it is never a silent no-op.
- **8.7.6** When energy outcomes are reported, they are joules per period and
  per work item, through the profiler summary (§11.7), beside the latency
  figures.

## 9. Capture and replay

- **9.1** When a sequence of capture-safe submissions (manifest §8.3) repeats
  with stable topology, geometry and buffer addresses, and will replay at least
  3 times, the runtime captures it once and replays it (measured crossover;
  Ekelund).
- **9.2** When a captured graph is sized, it is batched at a measured optimum
  (~50–100 nodes on A100-class hardware, essentially workload-independent) and
  never as the whole loop; above the platform's linear-creation regime (~2,500
  nodes) the runtime splits into multiple graphs.
- **9.3** When a captured graph's parameters change but its topology does not,
  the runtime updates in place rather than re-instantiating (~400 µs to
  instantiate on A100).
- **9.4** When capture is enabled for a sequence, its measured benefit is
  recorded per graph, and a graph that measures negative (short runs) falls
  back to live submission; the honest whole-application range on real solvers
  is 2–12 % (Diakun), so capture is a per-graph decision, never a global mode.
- **9.5** When the backend is HIP or CUDA, the mechanism is the vendor graph
  API; when it is Vulkan, it is a pre-recorded command buffer sequence with
  timeline semaphores (which also removes the wait-idle-per-dispatch defect
  filed as `vulkan-dispatch-serialization`); when it is CPU, it is a pre-built
  task list.
- **9.6** When a captured node reads an `indirect` launch bound, the bound is
  read on device at replay; data-dependent work counts do not break capture.
- **9.7** When the per-node cost of the runtime's own dispatch is measured, it
  stays under 1 µs (Ginkgo's whole framework dispatch is 1.0–1.5 µs per
  iteration); a degenerate one-element problem is in the benchmark suite so
  framework overhead is measured directly.
- **9.8** When per-item work is small relative to launch cost and the DAG's
  topology is fixed, the runtime may lower a kernel chain onto a persistent
  worker loop with device-side queues instead of discrete launches
  (Whippletree: up to 4× over successive launches); this is a variant the
  manifest names, selected by measurement.

## 10. Resident pools

- **10.1** When a workload holds device state that is large, long-lived across
  many launches, growing, or shared by reference (KV caches, weight sets,
  streaming ring buffers, cross-attention keys), it declares a **resident
  pool** with a capacity, a block size, and an eviction policy; the pool is a
  scheduler-visible resource, not an allocator detail.
- **10.2** When a submission reserves pool blocks, admission checks the
  reservation against free blocks plus the declared headroom before the
  submission is queued; the Orca deadlock (no space for the next step's state)
  is prevented at admission, not discovered at launch.
- **10.3** When items in a pool are gang-scheduled (beam candidates, a
  sequence group), eviction is all-or-nothing per gang.
- **10.4** When a pool is exhausted, the submission is rejected or queued with
  a reason; the pool's recompute-versus-spill choice is driven by measured
  costs per pool (vLLM: recompute never exceeds 20 % of swap latency at small
  blocks).
- **10.5** When a pool's block size couples to the subgroup width (one wave
  per block), the pool declares it and the manifest's wave width is checked
  against it.
- **10.6** When a pool is declared for a workload whose shapes are static
  (vision, training), paging indirection is not applied device-wide; paging is
  a per-pool policy (vLLM's scope warning).
- **10.7** When device memory is managed for the scheduler, it is a pool of
  fixed-size pages so the scheduler's memory state is one free-page count per
  pool (Clockwork), and no driver allocation occurs on the submission path
  (PipeSwitch).

## 11. Feedback

- **11.1** When the profiler's record sink delivers dispatch records, the
  scheduler's registered sink consumes them as a lossy, sampled correction
  (`cajeta-profiler` §5.6.4: drop, never block) — never as a ledger.
- **11.2** When a record arrives, its duration lands in a bounded rolling
  window keyed by manifest identity and geometry bucket; predictions use a
  high percentile of the window, and over- and under-prediction are tracked
  separately with 99th-percentile under-prediction held under 100 µs for
  predictable kernels (Clockwork: 144 µs over, 55 µs under, compounding ~4×).
- **11.3** When a co-residency pair measures worse than predicted, the pair is
  demoted; when it measures better, it is promoted; both are recorded in the
  calibration set (§12).
- **11.4** When admission thresholds are tuned online, they move by a bounded
  step at fixed epoch boundaries, never per launch (MASK's controller shape).
- **11.5** When the runtime reports scheduling outcomes, the primary metrics
  are goodput (items completed within deadline), per-kernel slowdown relative
  to solo (unfairness is the maximum over kernels), weighted speedup, tail
  latency, queue-empty time, and throughput per watt where power is readable;
  aggregate occupancy is never reported as a win (Salus: makespan, not
  occupancy).
- **11.6** When a utilization figure is printed, the definition used
  (accelerator-only, host-inclusive, combined) is stated with the number.
- **11.7** When the profiler summary is printed, it carries one line per
  kernel and shape from the calibration set: derived class, achieved fraction
  of the measured roofline, duration percentile, predictability, bytes moved —
  so a developer sees what the scheduler inferred and fixes a kernel that
  measures badly instead of describing it (manifest spec §14.4).

## 12. The calibration set — per device, measured, persisted

- **12.1** When the runtime first meets a device (or its driver or the compiler
  version changes), it measures and stores: peak matrix-core and vector
  throughput, achievable bandwidth (idle and under host load on an APU), the
  ridge point, bytes in flight needed to saturate bandwidth, per-backend launch
  cost `β` and per-item cost slope `α` for a reference kernel, event cost,
  graph node creation and instantiation cost, inter-queue dispatch delay, the
  scheduling-delay coefficients versus resident count, yield and (where any)
  preemption latency, and the power-droop coefficient where power is readable.
- **12.2** When a fact cannot be measured on a device, the set records it as
  absent and the policies that need it degrade (§5.8, §6.6); no literal from
  another device is substituted (`TargetDescriptor`'s rule).
- **12.3** When a kernel is first co-run, its scaling knee (§7.3) and its
  sensitivity to last-level-cache pressure are measured and stored under its
  manifest identity.
- **12.4** When the calibration set is held, its source of truth is **memory**
  (tier 1): every process measures each shape once, solo, and schedules from
  that; this works with no writable filesystem, in a sandbox, in a notebook
  session, on an embedded target. The shipped `DeviceProfile` (per-process,
  unpersisted) is extended to hold it rather than replaced.
- **12.4a** When calibration records leave memory, they do so only through a
  **`CalibrationStore`**, the I/O module — the single mechanism (developer,
  2026-09-06: "a discovery class is the only way to go"). The interface has
  three operations: load a record by key, store a record by key, and report
  whether the store is writable. Memory is the scheduler's own state, not a
  store; the store is where records come from at startup and go to when
  measured. An application **replaces** the default module with its own before
  the first launch when it knows its platform better than the runtime does (a
  console's save-data API, a flash region, a shared network cache). No
  environment variable or property name is part of the contract; a module that
  wants one reads it itself.
- **12.4b** When no module is installed, the **default module implements the
  heuristic**: it first serves read-only records embedded in the artifact at
  build (§12.4c) if any, then probes the platform's user cache convention once
  for writability (the XDG cache directory or `~/.cache` on Linux, local
  application data on Windows, the user caches directory on macOS — never the
  artifact store, which is a content-addressed registry, and never `/tmp`) and
  reports itself read-only or absent if nothing is writable. Records are keyed
  by manifest hash, device identity, driver version, compiler version and
  **power mode**; a mismatched or corrupt record is ignored; a failed store
  never fails the program; writes are deferred, atomic and bounded; the cache
  is disposable because measurement regenerates it. A cost table captured at
  one power mode is wrong by 2.1× at another (cuRobo: the same twenty kernels
  ran 316 µs at 60 W and 662 µs at 15 W on Orin), so a power-mode change
  invalidates the record.
- **12.4c** When a deployment has no writable storage and a fixed target
  (opt-in), a snapshot produced by `cajeta calibrate` on that device is
  embedded by `cajeta build` as read-only data beside the manifest and served
  by the default module; at runtime it is validated against the live device
  identity and discarded on mismatch, leaving memory alone. **The default
  ships nothing measured** (developer, 2026-09-06): the language is platform
  independent, and a binary carries no assumption about the device it lands on.
- **12.4d** When the module faults, the scheduler isolates it, disables it,
  reports it once, and continues from memory (the profiler's sink discipline,
  §5.6.5 there); no store operation sits on the launch path.
- **12.6** When the set is first built, it includes a two-point occupancy
  headroom probe (a solver-shaped chain at 4 and at 48 concurrent units) so the
  runtime knows whether co-running is viable on this device at all: the probe
  cost 8 ms on a 4090 and 417 ms on an Orin at 15 W (cuRobo).
- **12.7** When power is readable, the set includes a **picojoules-per-
  operation table by precision** (matrix-core and vector paths at each stored
  width the backend supports, plus per-byte off-chip traffic cost), measured
  by sampling power under a synthetic kernel family, and the device's base
  power; the sampling latency of the power source (10–100 ms on `rocm-smi` and
  NVML) is recorded with it so §8.7's estimates carry their own uncertainty.
- **12.5** When the number the AMD arc measured (a 2.2 µs in-stream launch gap
  on gfx1151) and the number the profiler measured on Ada (~8 µs per probe
  submit) are compared, they are first made commensurable by measuring both
  quantities on both devices with one method; neither is inherited as a
  constant.

## 13. Backend mechanisms and the degradation ladder

| Mechanism | AMDGPU / HIP | NVPTX / CUDA | Vulkan / SPIR-V | CPU |
|---|---|---|---|---|
| Multiple streams or queues | exists | exists | one queue today; multi-queue + timeline semaphores required (prerequisite: fix `vulkan-dispatch-serialization`) | worker pool |
| Stream priority (hint only, never for ordering) | `hipStreamCreateWithPriority` | `cudaStreamCreateWithPriority` | queue priority, `VK_KHR_global_priority` | n/a |
| Spatial partition | per-stream CU mask | green contexts (12.4+, Ada) | none | worker cap |
| Capture and replay | `hipGraph` | `cudaGraph` | pre-recorded command buffers | task list |
| Events and fences | exists; AQL barrier bits for in-queue dependencies without a host round trip | exists | timeline semaphores | exists |
| Yield | group-boundary flag (compiler) | same | same | same |
| Preemption from userspace | none (CWSR is driver-internal) | none (compute preemption serves the watchdog and debugger only) | none (`REALTIME` global priority is privilege-gated where enforced) | n/a |
| Completion notification ring | lowering pass | lowering pass | lowering pass | direct |

- **13.1** When a mechanism is absent on the active backend, the runtime takes
  the next rung (partition → rate control → queue placement → single queue) and
  records which rung it took; it never fails a submission for a missing
  mechanism.
- **13.2** When the CPU backend is active, the scheduler degrades to ordinary
  task scheduling over the worker pool, and every policy remains testable there
  without a device (the profiler's CPU-emulation tier makes records exact).

## 14. Budgets

Every number below is pinned as a design target with its source; each is
re-measured on both reference devices by the calibration set (§12) before it is
relied on. **Our own measurements live in
[`xpu-tile-scheduling-report`](xpu-tile-scheduling-report.md)** (developer,
2026-09-06: numbers driven) — a baseline profiled before any scheduler code,
one trial row per configuration tried, and a "not worse beyond noise" gate on
every optimization unit of the three plans. A literature number in this
table is never a substitute for a row in that report.

| Budget | Target | Source (silicon) |
|---|---|---|
| Scheduler decision cost | < 10 µs | Paella (Xeon Silver 4114) |
| Per-node cost inside a replayed graph | < 1 µs | Ginkgo (1.0–1.5 µs framework dispatch) |
| Instrumentation cost at 160 groups | ≤ 8 µs | Paella (T4, 6.6 µs) |
| In-flight descriptor depth | 4 | REEF (MI50) |
| Launch cost, uncaptured | 2–10 µs; 9.6 sync / 3.8 overlapped / 3.4 graph vs 2.9 floor | GROMACS (H100-class); NVIDIA microbenchmark |
| Event or dependency call | < 1 µs | GROMACS |
| Graph replay crossover | ≥ 3 replays | Ekelund (A100) |
| Graph batch size | ~50–100 nodes; split above ~2,500 | Ekelund, Diakun (A100) |
| Graph node creation; instantiate | ~4.2 µs/node + 0.16–0.42 ms; ~400 µs | Ekelund (A100) |
| Whole-application capture gain, real solvers | 2–12 %, can be negative | Diakun (NAS CG/LU, A100) |
| Preemption, end to end | ~100 µs (44.3 MB context); 275 µs via context time-slice group | GPreempt (A100); XAuto (Jetson Orin) |
| Smallest unit scheduled individually | ≥ 100 µs; shorter work is a captured chain | XAuto (~10 % overhead at 100 µs nodes) |
| Capture gain on a short-kernel chain | 10× (20 kernels, 1–12 µs each) | cuRobo (RTX 4090) |
| Host overhead ceiling | ≤ 15 % of device time | cuRobo |
| Scheduler CPU share | < 0.5 % for a 40-node graph | XAuto |
| Co-run penalty on the protected task | +230 % unpartitioned; +75 % time-sliced; +3 % at 90/10; +46 % at 50/50 | manipulation study (Orin) |
| Yield-check spacing | 10–50 µs of work | derived (§8.4.6) |
| Cost-history window | 5 samples; utilization sampled at ≤ 10 ms | DARIS; manipulation study |
| Energy per multiply-add by precision | 0.22 pJ at 3-bit; 1.76 pJ at 8-bit (8×) | edge-robotics energy survey |
| Share of energy in off-chip traffic | ~50 % | edge-robotics energy survey |
| Energy-constrained scheduling, achievable | 4.6× lower at equal latency (630 → 136 mJ, 25 ms) | Map-and-Conquer (edge SoC) |
| Power-mode cost shift | 2.1× (60 W → 15 W, same kernels) | cuRobo (Orin) |
| Power sampling latency | 10–100 ms | `rocm-smi`, NVML |
| Protected p99 under co-run | ≤ 15 % over solo (measured 9–22 %) | Orion (V100/A100), REEF (MI50) |
| Frame time under co-run | ≤ 10 % over solo | TGS |
| Best-effort outstanding work | ≤ 2.5 % of protected budget | Orion |
| Rate-control overhead on a solo stream | ≤ 5 % | TGS (0.3–5 %) |
| Interference model profiling | ≤ 11 configurations, ≤ 5 min per kernel | iGniter |
| Interference estimate error | ≤ 15 % at a 4-kernel mix | iGniter (1.5–5 %) |
| Async-compute gain to plan for | 1.1–1.9 ms per 1080p frame; "up to 10 %" | Interplay of Light (RTX 3080M); AMD/NVIDIA |
| CU-cap recovery on async co-runner | up to 1.5 ms per frame | DOOM (PS4 GCN) |
| Aliasing-barrier cost | ~5 % of frame | Halcyon (PC drivers) |
| Audio frame period | 10.7 ms (24 kHz, 256-sample hop) | profiles spec §3 |
| Decode slowdown from an unchunked prefill in the batch | 3–6× single; up to 28× | DistServe, Sarathi |
| Tile-granularity miss | +32 % for one element | Sarathi |

## 15. Integration

- **15.1** When the cooperative-tile seams are adopted, `KernelSubmission`
  gains the fields of §2 and loses author-filled footprint; every launch goes
  through the scheduler (§2.1) and `Scheduler.submit` is the explicit form of
  the same path; the `KernelResourceDescriptor` it returns is the manifest
  bound to the submission; `SchedulePolicy` keeps its ordinals and appends
  `Energy` as 3; `ArithmeticCharacter` keeps its ordinals and becomes a hint.
- **15.6** When the Vulkan backend is scheduled, the wait-idle-per-dispatch
  fix (`vulkan-dispatch-serialization`) ships first as its own plan; this
  arc's Vulkan unit then builds multiple queues, timeline semaphores and
  pre-recorded submission on top of it (developer, 2026-09-06).
- **15.2** When cajeta-llm's request scheduler (`dev.cajeta.llm.sched.Scheduler`,
  continuous batching with chunked prefill) runs on this scheduler, it remains
  the request-level policy and becomes a client: each iteration it assembles is
  one ragged submission with the token count as the work axis (profiles spec
  §3). cabra's serving layer is unchanged.
- **15.3** When the profiler is present, the scheduler registers a sink with
  per-record granularity; when it is absent, the scheduler runs on manifests
  and calibration alone and says so.
- **15.4** When the `xpu-kernel-scheduling-hints` surface is used inside a
  kernel, it is orthogonal: it shapes instructions within one kernel; this spec
  shapes kernels within a device.
- **15.5** When `xpu-gfx-streaming-geometry` refers to the orchestrator's `GFX`
  class, it now refers to the `frameBudget` policy and the game profile.

## 16. Risks

1. **Concurrency is not guaranteed by streams.** Two streams may serialize on
   a backend; §5.9's measure-and-revert bounds the damage, and the calibration
   set records the backend's measured concurrency.
2. **Interference-model error.** Mispredictions violate deadlines; §5.8
   refuses under uncertainty and §11 corrects online.
3. **Vulkan.** Multi-queue, timeline semaphores and pre-recorded submission do
   not exist in the backend today; the game profile is blocked on them.
4. **Instrumentation cost.** The completion ring's cost is a hard budget; if a
   backend cannot meet it, that backend runs without notifications and §6.8
   degrades to duration prediction.
5. **Power readability.** `rocm-smi` and NVML sample at 10–100 ms; the power
   channel is coarse and may be unavailable in CI. With `energy` in v1 this
   is now a first-order risk: the picojoule table's error bounds are part of
   the calibration set (§12.7) and the policy declares itself unavailable
   rather than guessing (§8.7.5).
6. **The APU interaction.** Host-side overlap steals GPU clock on a shared
   package budget (Nexus's 7.4× lever meets iGniter's power channel); no paper
   measures it, and it is an early experiment.

## 17. Decisions

- **D1 — kernel granularity, single process, complete access sets at submit.**
  Salus's objections to kernel-granularity scheduling (central bottleneck,
  broken batching, progressive-allocation deadlock) are consequences of
  scheduling beneath an uncooperative framework; owning the seam removes them.
  Salus's cost objection stands and becomes the 10 µs budget.
- **D2 — no MIG, no MPS, no driver-level preemption.** CU masks and green
  contexts are the partition mechanisms; group-boundary yield is the only
  stop; admission does the rest.
- **D3 — class derived per submission; author tag is a hint.**
- **D4 — rate control is always on; the analytical model is for deadlines.**
  TGS's zero-profile controller matched AntMan within 4 %; iGniter's model is
  reserved for admission of deadline-carrying work.
- **D5 — CU mask plus rate control together.** No paper evaluates the
  combination; it is this arc's first experiment.
- **D6 — capture is per-graph and measured.** Never a global mode.
- **D7 — the value-typed submission, not a launch thunk.** The lambda-launch
  defect stays open as a language issue; the scheduler does not depend on it.
- **D8 — the game profile is compute-side.** The Vulkan backend rasterizes
  nothing; the profile covers a renderer's compute passes and the frame
  discipline, with graphics-queue interop deferred (profiles spec §2.1).
- **D9 — the kernel gap catalog is retired.** Kernel families each profile
  needs are listed in the profiles spec without a status column; status lives
  in the specs that build them.
- **D10 — measured, in memory, nothing shipped by default.** (Developer,
  2026-09-06.) No author-declared cost anywhere; the calibration set's source
  of truth is memory; a cache and an embedded snapshot are accelerators, the
  latter opt-in for fixed hardware only.
- **D12 — every launch is a submission.** (Developer, 2026-09-06.) No
  separate direct path; the existing launch form is a default-policy
  submission; a bypass switch exists for measurement only.
- **D13 — Vulkan is split.** (Developer, 2026-09-06.) The serialization fix
  ships first on its own; this arc owns the queue model on top of it.
- **D14 — `energy` is a v1 policy.** (Developer, 2026-09-06.) A joules-per-
  period constraint on periodic streams, from a measured picojoule table by
  precision; precision degrades before rate; unavailable where power is not
  readable, never a silent no-op.
- **D15 — validation order is ML, then simulation, then game.** (Developer,
  2026-09-06.)
- **D11 — one storage mechanism: the `CalibrationStore` I/O module.**
  (Developer, 2026-09-06.) The default module implements the heuristic
  (bundled snapshot, then platform cache); an application replaces it with a
  custom module. Memory is the scheduler's state, not a store. No environment
  variable or property name is a runtime contract.

## 18. Open questions for the developer

- **O1 Persistence of the calibration set — resolved 2026-09-06** as the
  `CalibrationStore` I/O module of §12.4a–12.4d: memory is the scheduler's
  state; the default module implements the heuristic (embedded read-only
  snapshot when built in, then the platform cache when writable) and an
  application replaces it with a custom module; the discovery class is the
  only mechanism and no environment or property name is part of the contract.
  Nothing measured ships by default.
- **O2 Scheduled-launch spelling — resolved 2026-09-06** as D12: every launch
  is a submission; the existing form is the default-policy case.
- **O3 Vulkan queue work ownership — resolved 2026-09-06** as D13: the
  serialization fix first as its own plan, then this arc's Vulkan unit.
- **O4 Power and energy — resolved 2026-09-06** as D14: the `energy` policy
  is implemented in v1 (§8.7, §12.7), not reserved.
- **O5 Validation order — resolved 2026-09-06** as D15: ML, then simulation,
  then game.

## 19. References

- [`xpu-tile-scheduling-findings`](xpu-tile-scheduling-findings.md) — the
  synthesis of five corpus readings with every number's source.
- [`xpu-tile-scheduling-report`](xpu-tile-scheduling-report.md) — the numbers
  ledger: method, measured device facts, baseline, trials, residuals, closing
  summary.
- Corpora with per-paper markers: `research/xpu-scheduling/papers/` (19),
  `research/llm-serving/papers/` (12), `research/robotics-edge/papers/` (10),
  `research/gfx-scheduling/` (11 PDFs + `SOURCES.md`),
  `research/sim-scheduling/` (13 PDFs + `SOURCES.md`).
- Sibling specs: [`xpu-tile-manifest`](xpu-tile-manifest-spec.md),
  [`xpu-tile-workload-profiles`](xpu-tile-workload-profiles-spec.md),
  [`xpu-cooperative-tile`](xpu-cooperative-tile-spec.md),
  [`cajeta-profiler`](cajeta-profiler-spec.md) §5.6,
  [`xpu-kernel-scheduling-hints`](xpu-kernel-scheduling-hints-spec.md),
  [`kernel-artifact-inspection`](kernel-artifact-inspection-spec.md),
  [`xpu-gfx-streaming-geometry`](xpu-gfx-streaming-geometry-spec.md).
