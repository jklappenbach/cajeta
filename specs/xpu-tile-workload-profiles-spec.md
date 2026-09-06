# Spec: Tile workload profiles — game rendering, multimodal ML, ML training at scale, engineering simulation (`xpu-tile-workload-profiles`)

**active** — filed 2026-09-06, approved 2026-09-06 (every open question resolved with the developer; plan in `agents/`). The client half of the Tile scheduling family:
what each of the four workload classes the scheduler must serve looks like as
a stream of submissions, which policies it uses, what it needs from the
manifest ([`xpu-tile-manifest`](xpu-tile-manifest-spec.md)) and the scheduler
([`xpu-tile-scheduling`](xpu-tile-scheduling-spec.md)), and the witness
pipelines that prove the scheduler serves it. Absorbs the workload halves of
[`llm-kernel-scheduling`](archive/llm-kernel-scheduling-spec.md) and
[`robotics-kernel-scheduling`](archive/robotics-kernel-scheduling-spec.md).
Research record: [`xpu-tile-scheduling-findings`](xpu-tile-scheduling-findings.md).

## 1. Definition

### 1.1 Purpose

A scheduler validated on one workload becomes that workload's scheduler. The
cooperative-tile arc learned this and demanded two clients from day one; this
family demands four, chosen because they pull in different directions: a
renderer wants a fixed period and determinism, a multimodal ML stack wants
throughput for one model and latency for two others on the same device, a
training job wants every matrix core busy for hours with a second job
harvesting the gaps, and a simulation wants thousands of identical steps with
nothing dropped. Each
profile below is a set of conformance requirements the scheduler must meet, a
list of what the profile needs the manifest to carry, and the witness pipeline
that measures it.

### 1.2 Scope

- **In:** the four profiles; their kernel-class tables; their deadline and
  batching models; their resident-state needs; the witness pipelines and
  budgets; the cross-profile conformance rules; how the retired robotics
  profile's multi-rate model folds in.
- **Out:** the kernels themselves (each profile lists the kernel families it
  needs; building them belongs to the specs that build them); rasterization
  and graphics-API interop (the Vulkan backend is compute-only); cluster
  serving (cabra and `llm-serving` own that).

### 1.3 Principles

- **A profile is a conformance suite, not a feature.** Each profile's
  requirements are testable against the scheduler on the AMD and CPU backends.
- **No domain vocabulary enters the scheduler.** Tokens, frames and time steps
  are all "work items on a work axis" to the scheduler; the profile maps its
  vocabulary onto the seam.
- **Witnesses measure, never assert.** Every profile closes with a measured
  witness and a pinned budget.

## 2. Game rendering pipelines (compute side)

### 2.1 Shape

A frame is a **render graph**: passes declared with their resource accesses,
compiled into an order with derived barriers, executed against a fixed period.
Cajeta hosts the **compute passes** of such a renderer (GPU-driven culling and
compaction, light binning, ambient occlusion and screen-space effects, particle
and physics simulation, post-processing, an ML upscaler or denoiser) and the
frame discipline around them; the raster passes live in the host engine's
graphics queue and reach cajeta through buffer interop, which is deferred
(scheduling spec D8).

### 2.2 Kernel classes

| Pass family | Class | Notes |
|---|---|---|
| Cluster and instance culling, compaction | memory-bound; launch-bound at small scenes | needs scan and compaction primitives |
| Light binning, tile classification | memory-bound | scatter into tiles |
| Screen-space AO, reflections, GTAO | ALU-bound (compute-bound) | pairs with bandwidth-bound passes |
| Blur, bloom, tone map, post chain | bandwidth-bound | many small passes; capture candidate |
| Particles, cloth, fluid | mixed; particle N-body compute-bound | step-like inside a frame |
| ML upscaler or denoiser | matrix-core-bound | `Tile` fragments; the async partner for bandwidth passes |
| Barrier and transition cost | none (fixed) | ~5 % of frame from aliasing barriers alone (Halcyon) |

### 2.3 Deadline model

- **2.3.1** When a frame stream is declared, it names a period (16.67 ms at
  60 Hz; mobile-class targets reserve 2–3 ms headroom) and its budget is split
  by measurement into CPU and GPU halves that are reported separately.
- **2.3.2** When the frame graph is compiled, every barrier and transition is
  derived from pass access declarations; no pass writes one, none is emitted
  for a resource already in state, and split barriers hide latency where
  possible (FrameGraph, RDG, Arntzen).
- **2.3.3** When two transient resources have disjoint live ranges, they may
  share memory, planned across the frame timeline rather than greedily; the
  aliasing-barrier cost is measured and aliasing is disabled when it exceeds
  the memory saving.
- **2.3.4** When a pass's output is never read by a surviving pass, the pass is
  culled.
- **2.3.5** When the frame cannot hold its rate, the stream steps down a
  declared rate ladder (90 → 60 → 45 → 30) and reports it; when a subsystem is
  over budget, it sheds work through its degraded-variant family (LOD) rather
  than being dropped whole.

### 2.4 Queue model and co-running

- **2.4.1** When the backend offers more than one queue, the default is one
  graphics queue (host-owned), one compute queue and one copy queue; extra
  compute queues are not created without measured benefit (AMD: none seen).
- **2.4.2** When a pass is marked async-eligible, its cross-queue wait is placed
  after its last producer and its signal before its first consumer,
  automatically (RDG).
- **2.4.3** When co-runners are chosen, they are paired by bottleneck unit
  (ALU, bandwidth, L1/L2, tensor, geometry front-end), never by occupancy;
  measured effect of a good versus bad pairing: 2.10 ms versus 3.22 ms for the
  same GTAO pass (RTX 3080 Mobile).
- **2.4.4** When a best-effort pass co-runs with frame work, its compute-unit
  allocation is capped (scheduling §7, §8.3.6).
- **2.4.5** When the same frame graph is submitted with the same inputs, queue
  and partition placement are identical frame to frame (scheduling §8.3.5).
- **2.4.6** When a cross-queue dependency is emitted, it is priced at one
  submission (a fence costs about what a submit costs), so passes are grouped
  large enough that the gain exceeds the fence.
- **2.4.7** When work has no dependency on the current frame (next frame's
  culling, a background ML pass), it is eligible to start during the current
  frame's tail.
- **2.4.8** When a best-effort co-runner is admitted, it runs under a
  compute-unit reservation whose ratio is the profile's principal knob
  (scheduling §5.12–5.13): unreserved co-running measured +230 % on the
  protected task, worse than serializing; a 90/10 reservation measured +3 %.
- **2.4.9** When the frame's terminal pass (composite or present) is
  dispatched, it is promoted one level (scheduling §8.5.1); a missed present is
  a dropped frame no matter how early the G-buffer finished.
- **2.4.10** When frame jitter is measured, the targets to hold are a 99th
  percentile at or under 1.25× the budget and a frame-to-frame delta at or
  under 2 ms at 60 Hz; both are pinned by the witness, not asserted.

### 2.5 What the profile needs from the manifest and scheduler

- **2.5.1** Access modes per pass with `write` meaning discardable previous
  contents (manifest §6.1), so `DONT_CARE` loads are derivable.
- **2.5.2** Degraded-variant families for LOD-style shedding (manifest §10).
- **2.5.3** The `frameBudget` policy with forbidden zone, early drop, pacing
  and the rate ladder (scheduling §8.3).
- **2.5.4** Capture and replay for the post chain, whose topology is stable
  across frames (scheduling §9).
- **2.5.5** Vulkan multi-queue with timeline semaphores (scheduling §13; blocked
  today).

### 2.6 Witness

- **2.6.1** When the game witness runs, it is a compute frame graph of at
  least twelve passes (culling → binning → AO → post chain → upscale) with
  declared accesses, a 16.67 ms period, and a best-effort matrix-core kernel
  (a `Tile` GEMM standing in for a background ML model) submitted under
  `throughput`; measured on AMD and CPU.
- **2.6.2** When the witness is measured, frame time with the co-runner stays
  within 10 % of solo frame time; the uncontrolled baseline (direct launches,
  no scheduler) is recorded alongside; barrier count is on the order of twice
  the written surfaces; placement is identical across 1,000 frames.
- **2.6.3** When the frame is deliberately overloaded, the witness shows the
  forbidden zone cutting the best-effort kernel, the degraded variant of the
  AO pass substituting, and the rate ladder stepping, each reported.

## 3. Heavy multimodal ML

### 3.1 Shape

One process holds several models at once — an LLM (prefill and decode), a
speech recognizer (STT), a speech synthesizer (TTS), a vision encoder (CNN or
ViT), and possibly a training loop — and streams their kernels through one
scheduler. Three of them have opposite characters at the same moment: LLM
decode is a memory-bound, best-effort throughput stream; STT and TTS are
periodic with hard audio-frame deadlines; a vision encoder or a prefill chunk is
a compute-bound single shot. This is the profile where the scheduler's whole
argument (co-run opposite classes, protect the periodic ones) is tested
hardest.

### 3.2 Kernel classes

| Kernel | Prefill (T = chunk) | Decode (T = batch) | Notes |
|---|---|---|---|
| Norms, RoPE, residual adds, activations | memory-bound | launch-bound | fuse into neighbours |
| Projection and MLP GEMMs | compute-bound above `T_crossover` | memory-bound | the crossover is the roofline coordinate |
| Attention | compute-bound (≈3,000 FLOP/byte) | memory-bound (≈`2g/b_kv`) | ~400× swing on `l_qo` alone |
| KV scatter | memory-bound | memory-bound, tiny | |
| MoE router, dispatch, combine | memory-bound | launch-bound | permutation, not matmul |
| MoE expert GEMMs | compute if `T·k/E` exceeds crossover | launch-bound swarm (~0.3 rows/expert at T = 20) | indirect bounds; capture |
| LM head | compute-bound | memory-bound, largest single read | |
| Sampling | memory-bound | launch-bound | |
| STT encoder (conformer/Whisper) | compute-bound, static shape | — | prefill-shaped |
| STT decoder | — | memory-bound over a static cross-attention KV | third KV regime |
| TTS acoustic (AR) / (diffusion) | — / compute-bound fixed steps | memory- and launch-bound | |
| TTS vocoder | bandwidth-bound at streaming chunk | — | the compute partner for decode |
| CNN convs (batch 1) / ViT encoder | memory- or launch-bound / compute-bound | — | static shapes; capture |
| Backward pass | ~2.5× forward FLOPs | — | accumulating writes (`dQ`) |
| Optimizer step | memory-bound, one fused launch | — | ~24 bytes/param/step |

### 3.3 The roofline coordinate

- **3.3.1** When an LLM iteration is submitted, its work axis is the total
  token count `T` in the launch — not batch size, not request count — and the
  manifest names it (manifest §4.6).
- **3.3.2** When a weight-stationary GEMM is classified, the crossover
  `T = R · b_w / 2` is computed from the measured ridge `R` and the stored
  weight width `b_w`, and the practical crossover is measured per kernel family
  because it runs 2.5–3× the algebraic one; a 4-bit model's crossover is a
  quarter of an fp16 model's, so policies tuned on fp16 literature over-admit.
- **3.3.3** When attention is submitted, the submission's arguments carry
  `(l_qo, l_kv, g)`; the manifest's expression classifies prefill, decode and
  speculative verify (`l_qo = γ + 1`) without a special case.

### 3.4 Batching as a scheduling policy

- **3.4.1** When an iteration is assembled, it is one ragged submission over
  non-uniform items (manifest §7.1) with a cost-balanced assignment of items to
  groups; skewed lengths otherwise lose 38 % of per-token latency to balance
  alone.
- **3.4.2** When a prefill is admitted into a decode iteration, it is chunked
  to a token budget `τ` (512 strict, 2,048 relaxed, measured per model and
  device) and the iteration is filled decode-first, then partial prefills, then
  new work; a whole prefill in a decode batch costs 3–6× (one) to 28× (long
  context) on time between tokens.
- **3.4.3** When every chunk is sized, it is rounded to the manifest's tile
  granularity (257 versus 256 tokens costs +32 %).
- **3.4.4** When prefill and decode co-run, they are granted disjoint
  compute-unit sets and the tile plan is recomputed against the grant
  (scheduling §7, FlashInfer's plan-time SM count); sharing a batch instead
  inherits the compute-bound kernel's latency.
- **3.4.5** When decode batch occupancy is designed for, the design point is
  `T ≤ 20` active tokens (production traces: 60–70 % of the time fewer than 20,
  more than 20 % of the time a single token), not `T = 64`.
- **3.4.5a** When batching gains are estimated by shape, the corpus's measured
  ranges are the prior: autoregressive decode 1.6–3.6×, vision encoders
  1.0–1.16×, U-Net-style denoisers ~1.08× (manipulation study, DARIS);
  batching a vision encoder is not where the time is.
- **3.4.6** When cajeta-llm's request scheduler runs on this profile, it stays
  the request-level policy (admission by KV headroom, youngest-victim
  preemption, FIFO resume) and submits one ragged iteration per step; nothing
  in its policies moves into the kernel scheduler.

### 3.5 Resident state

- **3.5.1** When an LLM is served, its KV cache is a resident pool (scheduling
  §10) with block size coupled to the wave width, gang eviction per sequence
  group, and a measured recompute-versus-spill choice; effective utilization
  moved from 20 % to 96 % by paging, and batch size follows.
- **3.5.2** When several models are resident, their weight sets are one pool
  each, so admission of a fourth model is checked, not discovered by
  allocation failure.
- **3.5.3** When an STT decoder runs, its cross-attention keys are a static,
  read-only pool for the utterance; when a streaming encoder or a vocoder runs,
  its left-context and overlap-add state is a ring pool; neither is transient
  and neither grows.
- **3.5.4** When a training loop runs, activations live into the backward pass
  are marked so (manifest §4; `xpu-tile-manifest` records it) and
  checkpoint-recompute launches are scheduled with their own cost, never
  treated as free.

### 3.6 MoE and speculative decoding

- **3.6.1** When MoE experts are launched at decode, the launch bound per
  expert comes from the device-side routed count (manifest §7.2); the swarm is
  a capture candidate (scheduling §9) and a grouped-GEMM primitive taking
  per-group offsets is the kernel family it needs.
- **3.6.2** When speculative decoding runs, the draft chain (γ serial small
  forward passes) is captured, the verify launch is classified from
  `l_qo = γ + 1`, and overlap comes only across requests (one request's verify
  against another's draft chain), never within one.
- **3.6.3** When a memory-bound submission is on the device, the scheduler
  treats its idle compute as the budget speculation may spend, reduced 4× for a
  4-bit model (§3.3.2).

### 3.7 Speech deadlines

- **3.7.1** When a TTS vocoder stream is declared, its period is one audio
  frame (10.7 ms at 24 kHz with a 256-sample hop) under `frameBudget`; when a
  streaming STT encoder is declared, its period is its chunk duration
  (160–960 ms) under `frameBudget` with a real-time factor below one.
- **3.7.2** When STT, LLM decode and TTS share one device, STT and TTS are
  protected periodic streams and the LLM is best-effort under `throughput`;
  the mouth-to-ear budget (200–300 ms) is split across the three by
  optimization (scheduling §8.2.5), not evenly.
- **3.7.3** When a speech pipeline's kernels are static-shaped and many, they
  are captured; 300 uncaptured launches against a 10.7 ms frame is 10–30 % of
  the budget spent on nothing.

### 3.8 Training

- **3.8.1** When a backward kernel accumulates into a gradient buffer, its
  manifest mode is `accumulate` and no two such launches co-run (manifest
  §6.4).
- **3.8.2** When the optimizer step runs, it is one fused multi-tensor launch,
  memory-bound, and the preferred co-run partner for any compute-bound
  submission.
- **3.8.3** When a parameter's gradient is complete, its reduction or transfer
  is submitted immediately on a queue that runs concurrently with the next
  backward kernel (Megatron), not after the pass.
- **3.8.4** When a memory-bound elementwise op sits between partitioned GEMMs,
  it is duplicated rather than synchronized around.
- **3.8.5** When launches are reordered, each kernel draws from the RNG stream
  identity its manifest names; reordering never changes which stream a kernel
  reads.

### 3.9 Witness

- **3.9.1** When the ML witness runs, it is cajeta-llm decode (a 4-bit model)
  under `throughput` co-running with a synthetic periodic vocoder-shaped stream
  (bandwidth-bound, 10.7 ms period, `frameBudget`) and a prefill chunk
  (compute-bound, `latency`), on AMD; the CPU backend runs the same three
  streams with the profiler's exact host tier.
- **3.9.2** When the witness is measured, the periodic stream misses no frame
  over 10,000 frames, decode tokens per second stays within 15 % of solo, the
  prefill chunk's latency stays within its window, and the uncontrolled
  baseline is recorded alongside.
- **3.9.3** When the training witness runs, it is one transformer layer's
  forward, backward and fused optimizer step with the optimizer co-running
  against the next layer's backward GEMM; the measured step time beats the
  serialized step and the gradient buffers are bit-identical to the serialized
  run.

## 3A. ML training at scale

Added as a fourth profile by the developer (2026-09-06). The kernel-level
rules of §3.8 apply unchanged; this section adds the profile: the shape of a
training step, its memory discipline, its overlap opportunities, its
multi-job behavior, and its witness. Distribution across devices belongs to
`cajeta-ml-dist`; this profile schedules one device's share of a training job
and treats collectives as submissions on a queue.

### 3A.1 Shape

A step is forward, backward, and an optimizer update, repeated with fixed
shapes for the life of a run. The backward carries about 2.5× the forward's
operations (five matmuls against two in attention alone), parallelizes on a
different axis, and accumulates into gradient buffers. Activations live from
the forward into the backward unless recomputed; the optimizer is a pure
streaming pass over every parameter, memory-bound and dependency-free.
Realistic efficiency runs from 30 % of peak on a single device baseline to
about 72 % model-FLOP utilization with a fully fused attention path
(Megatron, FlashAttention-2), so the profile's KPI is achieved fraction of the
measured matrix-core peak per step, not occupancy.

### 3A.2 Kernel classes

| Kernel | Class | Notes |
|---|---|---|
| Forward GEMMs, attention | compute-bound at training batch | static shapes; capture |
| Backward GEMMs | compute-bound | `accumulate` writes into gradients |
| Backward attention | compute-bound; atomic `dQ` | never co-run two accumulators |
| Recompute of checkpointed activations | compute-bound | a scheduled launch, never free |
| Norm, activation, dropout, residual | memory-bound | duplicate rather than synchronize |
| Loss and logits | compute then memory | fuse cross-entropy into the logit GEMM |
| Gradient reduction or transfer | interconnect-bound | issued per parameter as it completes |
| Optimizer step | memory-bound, one fused launch | ~24 bytes per parameter per step; ~120 ms per 1B parameters at 200 GB/s |
| Data loading and host transfer | transfer-bound | pipelined a stage ahead (PipeSwitch) |

### 3A.3 Memory discipline

- **3A.3.1** When activations are live into the backward, the manifest marks
  the intermediate so (manifest §4), and fusion across that boundary is
  refused; when a layer is checkpointed, the recompute launch is scheduled
  with its own cost and reservation.
- **3A.3.2** When a job must be suspended or switched (a second job, a
  validation phase), the switch happens at the iteration's memory minimum, and
  the resident state copied is an order of magnitude smaller than the
  iteration's peak allocation (Salus, Gandiva).
- **3A.3.3** When parameters, optimizer state, gradients and activations are
  allocated, they are resident pools (scheduling §10) sized once at job start;
  no driver allocation occurs on the step path.
- **3A.3.4** When the device shares memory with the host, no device-to-host
  spill path is built; the allocator's retained cache is bounded instead
  (AntMan and TGS on an APU).

### 3A.4 Overlap

- **3A.4.1** When a parameter's gradient completes, its reduction or transfer
  is submitted immediately on a queue that runs concurrently with the next
  backward kernel (Megatron's rule); the scheduler tracks per-parameter
  readiness through the access sets.
- **3A.4.2** When the optimizer step runs, it is one fused multi-tensor launch
  and the preferred co-run partner of the next layer's backward GEMM, since the
  two are opposite-class by construction.
- **3A.4.3** When the next micro-batch's inputs are transferred, the transfer
  overlaps the current micro-batch's compute at a grouping whose fixed cost is
  under a configured fraction of the group's compute time; on unified memory
  the stage is elided.
- **3A.4.4** When a memory-bound elementwise op sits between partitioned
  GEMMs, it is duplicated rather than synchronized around.
- **3A.4.5** When gradients are accumulated across micro-batches, the
  accumulate writes serialize per buffer and the step's dependency graph stays
  one captured chain per micro-batch.

### 3A.5 Multi-job behavior

- **3A.5.1** When a training job is protected and an opportunistic job shares
  the device, the opportunistic job's launch rate is regulated against the
  protected job's iteration period (scheduling §6), and the protected job stays
  within 10 % of its solo throughput (TGS measured 89–95 % of exclusive; AntMan
  within 4 % of it).
- **3A.5.2** When a protected job's phases change demand (train, validate,
  host-only), the controller re-allocates within one control interval of the
  transition.
- **3A.5.3** When two high-occupancy training kernels would co-run
  unregulated, they are interleaved instead (Gandiva's measured −11 to −16 %
  from unregulated packing).

### 3A.6 Shape and determinism

- **3A.6.1** When a vocabulary or hidden dimension is chosen, it is padded to
  the manifest's tile granularity (Megatron: a multiple of 128).
- **3A.6.2** When launches are reordered, each kernel draws from the RNG
  stream identity its manifest names; dropout inside a partitioned region
  draws differently per partition, outside it identically.
- **3A.6.3** When a step is replayed as a captured graph and the accumulation
  order is fixed, gradients are bit-identical between live and captured runs;
  when the order is not fixed, the tolerance is declared, never discovered.

### 3A.7 Witness

- **3A.7.1** When the training witness runs, it is a four-layer transformer
  block training step (bf16 compute, fp32 master weights, activation
  checkpointing on two layers) with a fused optimizer, on AMD and CPU from one
  source, using `cajeta-ml`'s training core as the client.
- **3A.7.2** When the witness is measured, the step with optimizer co-run and
  gradient-ready reductions beats the serialized step, achieved matrix-core
  fraction per step is reported, gradients are bit-identical to the serialized
  run under a fixed accumulation order, and the captured step matches the live
  step.
- **3A.7.3** When the multi-job witness runs, a protected training job
  co-runs with an opportunistic inference stream; the protected job's
  iteration period stays within 10 % of solo and the uncontrolled baseline is
  recorded alongside.

## 4. Engineering compute simulation

### 4.1 Shape

A time step is a DAG of dependent kernels — stencil updates, sparse
matrix-vector products, reductions and dot products inside an iterative
solver, boundary exchanges, occasional dense solves or FFTs — executed
thousands to millions of times with identical topology and changing data. At
strong scale the cost is not compute; it is submission, synchronization and
queue starvation: a molecular-dynamics step issues ~20 launches and ~30 event
calls, which at peak rate is over half of host wall time (GROMACS). Nothing may
be dropped, kernels are usually not idempotent, and some deployments
(interactive, hardware-in-the-loop) carry a wall-clock budget per step.

### 4.2 Kernel classes

| Kernel family | Class | Notes |
|---|---|---|
| Stencil update, structured grid | memory-bound | interior/boundary split |
| SpMV, SpMM | memory-bound, irregular | solver core |
| Dot product, norm, reduction | launch-bound | solver inner loop; capture |
| Dense solve, GEMM blocks | compute-bound | multigrid coarse levels, batched |
| FFT stages | mixed | |
| Particle pair forces, N-body | compute-bound | |
| Neighbor list build, sort, compaction | memory-bound | periodic, not every step |
| Halo pack/unpack, transfer | memory- and transfer-bound | overlap target |

### 4.3 Capture and replay

- **4.3.1** When a step's DAG repeats, it is captured after its topology has
  been observed stable and replayed at least three times; the batch is sized
  at the measured optimum (~50–100 nodes) and split above the platform's
  linear-creation regime (scheduling §9).
- **4.3.2** When a step's parameters change (time step, tolerances) but its
  topology does not, the graph is updated in place.
- **4.3.3** When a replayed step's measured benefit is negative on a short run,
  live submission is restored for that graph and the decision is recorded.
- **4.3.4** When per-item work is small relative to launch cost (solver inner
  products), the DAG may be lowered onto a persistent worker loop
  (scheduling §9.8).

### 4.4 Overlap and priorities

- **4.4.1** When a kernel's inputs divide into an interior part and a
  boundary part that waits on a transfer, the runtime splits it so interior
  compute runs during the exchange, without the author hand-writing the
  overlap (GROMACS halo redesign).
- **4.4.2** When a kernel produces a value another device or rank waits on,
  it is markable so it dispatches ahead of same-stage local work.
- **4.4.3** When priorities are offered to the profile, there are at least
  three tiers — critical path, reduction/update, low-priority pruning — because
  a two-tier scheme left up to 10 % on the table (GROMACS).
- **4.4.4** When a transfer is scheduled, it is a non-preemptive blocking
  interval charged against the step budget; when overlapping transfers exceed
  the device's copy-engine count they are serialized rather than assumed
  concurrent.
- **4.4.5** When the step is fully device-resident, tens to hundreds of steps
  are enqueued before a host synchronization is required.
- **4.4.6** When a node drains the device (a global reduction, an all-reduce,
  a solver dot product feeding a scalar decision), it is declared a barrier
  node, and the runtime pre-stages a complementary kernel into its shadow; in a
  serial solver chain of memory-bound kernels that shadow is the only reliable
  co-scheduling window, and two memory-bound solver streams gain nothing from
  each other (MegBA's PCG loop).
- **4.4.7** When the step has stages at different natural rates (a fast
  integrator, a slower contact or neighbor update, a slower telemetry or
  visualization path), they are declared as periodic streams joined by sample
  and accumulate edges (scheduling §8.6.2), and no single stage is optimized in
  isolation beyond its share of the step.

### 4.5 No-drop discipline

- **4.5.1** When the profile is active, the drop policy is disabled; admission
  rejects at submit with a reason instead (scheduling §2.9).
- **4.5.2** When a kernel is not restartable (the normal case: global state
  changes during execution), kill-and-restart is never used; deadline pressure
  is served by admission and by yield-capable best-effort work only.
- **4.5.3** When a foreign call drains the queue (GPU-aware interop on every
  call), it is counted as a full barrier and reported; queue-empty time is a
  first-class metric of the witness.

### 4.6 Budgeted steps

- **4.6.1** When a step carries a wall-clock budget (interactive or
  hardware-in-the-loop), the step is a `frameBudget` stream with the step as the
  frame; the forbidden zone and the rate ladder apply, and degradation is by
  variant family (coarser solve, fewer iterations), never by dropping a kernel.
- **4.6.2** When a periodic control loop shares the device with a simulation
  (the robotics model: a small dense solve at a fixed rate beside a large
  perception or physics load), the control loop is a `latency` stream whose
  worst case is admission-tested against the step's duty cycle
  (scheduling §8.2.6), and the simulation is best-effort against it.
- **4.6.3** When a hardware-in-the-loop step is budgeted, the schedulable
  budget is the wall-clock step minus the declared fixed I/O latency
  (scheduling §8.6.1); actuation delay alone measured 58–68 ms on a 100 ms
  budget, and a runtime that schedules against the nominal step overruns every
  time. The step jitter target is a 99th percentile at or under 1.10× the
  budget.
- **4.6.4** When a simulation is offline (no period declared), the deadline
  apparatus is inert: the profile is `throughput` with capture and fusion, and
  no admission test, forbidden zone or promotion runs.
- **4.6.5** When the device is first used by this profile, the two-point
  occupancy headroom probe (scheduling §12.6) decides whether co-running is
  viable at all; on a small device a solver leaves no room and the profile
  runs serial with capture.

### 4.7 Witness

- **4.7.1** When the simulation witness runs, it is a conjugate-gradient solver
  on a structured-grid Laplacian (stencil SpMV, two dot products, three vector
  updates per iteration) for at least 10,000 iterations, live and captured, on
  AMD and CPU, plus a one-element degenerate problem that measures framework
  overhead directly.
- **4.7.2** When the witness is measured, the captured loop's per-iteration
  host cost is under 1 µs per node, the replay gain is recorded honestly
  (expected 2–12 %), queue-empty time is reported, and the solution is
  bit-identical between live and captured runs.
- **4.7.3** When the overlap witness runs, it is a two-domain stencil with a
  boundary exchange, split by the runtime into interior and boundary
  submissions; the measured step time beats the serialized step, and results
  are bit-identical.

## 5. Cross-profile conformance

- **5.1** When any profile's witness runs, it runs on the AMD and CPU backends
  from one source; NVPTX and Vulkan runs are recorded when hardware is present
  and marked absent otherwise (the cooperative-tile verification boundary).
- **5.2** When a profile names a kernel family it needs (scan, compaction,
  scatter, grouped GEMM, segmented reduce, stencil, SpMV, attention, norms,
  sampling, fused optimizer), the need is recorded here without a status; the
  spec that builds the family owns its status (`xpu-scan-primitive`,
  cajeta-llm, and future specs).
- **5.3** When two profiles are resident in one process (a game with an ML
  NPC, a simulation with a visualizer, a training job with an inference
  stream), the highest-criticality stream's policy governs admission and the
  rest fill best-effort; the scheduler composes profiles, it does not select
  one.
- **5.4** When a profile's budget in scheduling §14 is re-measured on a
  reference device, the profile's witness pins the new number and the table is
  updated with the source.

## 6. The retired robotics profile

The 2026-07 robotics draft described a fourth client: multi-rate,
mixed-criticality pipelines on a fixed edge budget with energy as an
objective. Its scheduling content folds into this family as follows, and the
rest is deferred:

- **6.1** Periodic tasks with release times, deadlines and precedence are the
  `frameBudget` and `latency` streams of scheduling §8, with the duty-cycle
  admission test of §8.2.6; the survey's task-chain model is §4.3 there.
- **6.2** Graceful degradation under overload (lower rate, quantized model) is
  the rate ladder and the variant family, and the order is **precision before
  rate**: OpenVLA's int8 variant lost 13.2 points of task success when it
  missed a 5 Hz contract and was the best variant once the rate held; a 440 ms
  inference against a 100–200 ms budget cost 50 points. Missing a rate costs
  more than losing precision.
- **6.2a** The staging DARIS used in place of hardware preemption — sync points
  between sub-tasks, whose removal cost 33 % throughput and 22.5 points of
  best-effort misses — is the group-boundary yield of scheduling §8.4, one
  level down.
- **6.3** Engine-affinity placement across GPU, NPU and real-time CPU cores is
  deferred to `npu-target-support`; the scheduler places on one device.
- **6.4** The energy governor is the `energy` policy of scheduling §8.7,
  decided into v1 (developer, 2026-09-06): a joules-per-period constraint on a
  periodic stream, degrading precision before rate. Any profile's periodic
  stream may declare it — a mobile-class frame stream, a battery-bound speech
  pipeline, an edge simulation step.

## 7. Decisions

- **D1 — four profiles, each with a measured witness on two backends.**
  (Developer, 2026-09-06: training at scale added as the fourth.)
- **D2 — the game profile is compute-side.** Raster interop is deferred; the
  frame discipline and the compute passes are the deliverable.
- **D3 — the ML witness is the first built** (scheduling D15): both reference
  devices, and cajeta-llm and cabra exist as clients.
- **D4 — cajeta-llm's request scheduler is a client, not a casualty.** Its
  policies stay; its iterations become ragged submissions.
- **D5 — robotics is folded, not kept.** Its model survives as the deadline
  streams and the `energy` policy; engine affinity waits for
  `npu-target-support`.
- **D6 — the speech witness is synthetic first.** (Developer, 2026-09-06.) A
  vocoder-shaped bandwidth-bound periodic stream beside cajeta-llm decode; a
  real STT/TTS model replaces it when a speech client exists.
- **D7 — the game witness runs on HIP streams first.** (Developer,
  2026-09-06.) The frame discipline is backend-independent; the Vulkan re-run
  follows the queue unit.
- **D8 — training at scale is a profile, with `cajeta-ml`'s training core as
  its client.** (Developer, 2026-09-06.) Distribution stays with
  `cajeta-ml-dist`.

## 8. Open questions — resolved 2026-09-06

- **O1** (speech witness) — resolved as D6.
- **O2** (game witness backend) — resolved as D7.
- **O3** (fourth profile) — resolved as D8: training at scale, §3A.

## 9. References

- [`xpu-tile-scheduling-findings`](xpu-tile-scheduling-findings.md).
- Corpora: `research/gfx-scheduling/` (FrameGraph, RDG, Unity SRP, Halcyon,
  idTech 6, Practical DX12, vendor async-compute guidance, GPreempt, work
  graphs, Whippletree), `research/sim-scheduling/` (GROMACS 2020 and 2025,
  Ginkgo, PETSc, StarPU, Legion, Realm, PaRSEC, Taskflow, Celerity, CUDA-graph
  measurements), `research/llm-serving/papers/` (Orca, vLLM, Sarathi-Serve,
  Splitwise, DistServe, FlashInfer, FlashAttention 1 and 2, speculative
  decoding, Megatron, Switch), `research/robotics-edge/papers/`.
- Sibling specs: [`xpu-tile-scheduling`](xpu-tile-scheduling-spec.md),
  [`xpu-tile-manifest`](xpu-tile-manifest-spec.md),
  [`xpu-gfx-streaming-geometry`](xpu-gfx-streaming-geometry-spec.md),
  [`xpu-scan-primitive`](xpu-scan-primitive-spec.md), cajeta-llm's engine
  spec, [`npu-target-support`](npu-target-support-spec.md).
