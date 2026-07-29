# Spec: LLM kernels & their scheduling (`llm-kernel-scheduling`)

## 1. Definition

### 1.1 Purpose

Characterize the **kernels a transformer LLM executes** and the **scheduling
they demand**, as the `ML_INFER` (and `ML_TRAIN`) workload the XPU orchestrator
([`xpu-kernel-scheduling`](xpu-kernel-scheduling-spec.md)) must serve. LLM serving
is the sharpest test of the orchestrator: its two execution phases have *opposite*
bottlenecks, its memory (the KV cache) is the scarce resource, and requests
stream continuously with latency SLOs — so batching, phase co-scheduling, and
KV-cache residency dominate achievable utilization.

### 1.2 Scope

- The **kernel taxonomy** of a decoder LLM: Attention, GEMM, Normalization,
  Activation, Routing (MoE), Communication (collectives), and Sampling.
- The **two-phase structure** — *prefill* (prompt, compute-bound) vs *decode*
  (autoregressive token generation, memory-bound) — and why it drives scheduling.
- **KV-cache management** (paged, shared) as the residency problem.
- **Batching & phase scheduling**: continuous/iteration-level batching, chunked
  prefill, prefill/decode disaggregation, statistical multiplexing, speculative
  decoding.
- **Parallelism**: tensor / pipeline / expert parallel and the collectives they
  inject into the schedule.
- The mapping onto the orchestrator's tiers and per-class policy.

### 1.3 Non-goals

- Not a training-systems spec (referenced for parallelism/collectives only).
- Not a specific model architecture; the kernel families are architecture-general
  (dense or MoE, MHA/GQA/MLA attention).
- Not the attention kernel's internal design (FlashAttention IO-tiling is a
  *consumer* here); this spec schedules whole kernels, not their inner loops.

### 1.4 Principles

- **Two phases, two bottlenecks — co-schedule them.** Prefill saturates matrix
  cores; decode saturates HBM bandwidth and leaves compute idle. Running them
  *together* (chunked prefill piggybacked with decode) is the biggest single
  utilization win (Sarathi-Serve); running them *apart* on matched hardware is the
  other valid answer (DistServe, Splitwise). Both beat naive interleave.
- **The KV cache is the scarce resource.** Batch size — and thus utilization — is
  bounded by KV-cache memory, not compute. Page it, share it, evict it
  (PagedAttention).
- **Batch at iteration granularity, not request granularity.** Sequences finish at
  different lengths; join/leave the batch every decode step to keep the GPU full
  (Orca continuous batching).
- **Requests are bursty and diverse — multiplex them.** Statistical multiplexing
  across GPUs/replicas cuts tail latency (AlpaServe); admission + SLO-aware
  ordering protect latency targets.
- **Decode is latency-bound; reshape it into compute where possible.** Speculative
  decoding turns sequential memory-bound steps into a batched verify — a scheduling
  lever, not just an algorithm.

### 1.5 Kernel status (current vs planned)

Of the decoder-layer kernels below, only the GEMM is built today.

- **Built (reusable here):** `matmulF32` (dense GEMM) — the projection/MLP GEMMs.
- **Must build (see the local `xpu-kernel-library` backlog + master-spec gap
  catalog):** `attention` (flash + paged KV), `softmax`/`LayerNorm`/`RMSNorm`,
  fused `activation` epilogues, `sampling`/top-k, `scatter` (MoE routing), and
  `collectives` (all-reduce / all-to-all for tensor/expert parallel).

## 2. The kernel taxonomy of a decoder layer

### 2.1 Requirement

Enumerate the kernels one transformer layer runs, with roofline class, so the
orchestrator can classify and co-schedule them.

### 2.2 Mechanism — one layer's kernels

| Kernel | Family | Roofline | Notes |
|---|---|---|---|
| Input **Norm** (LayerNorm/RMSNorm) | Normalization | memory-bound | per-token reduction + scale |
| **QKV projection** GEMM | GEMM | compute-bound (prefill) / memory-bound (decode) | |
| Positional (RoPE) | map | memory-bound | elementwise on Q/K |
| **Attention** (FlashAttention; paged for decode) | Attention | prefill: compute-bound; decode: **memory-bound** (KV reads) | KV-cache gather |
| **Output projection** GEMM | GEMM | compute/memory-bound | |
| Residual **add** | map | memory-bound | |
| Post **Norm** | Normalization | memory-bound | |
| **MLP up** GEMM + **activation** (GELU/SiLU, gated) | GEMM + map | compute-bound (prefill) | activation fusible into epilogue |
| **MLP down** GEMM | GEMM | compute/memory-bound | |
| *(MoE)* **router** + top-k + **all-to-all** + expert GEMMs | Routing + Communication | gather/scatter + comm + compute | dynamic, load-imbalanced |
| *(tensor-parallel)* **all-reduce** after attn/MLP | Communication | network/interconnect-bound | serializes with compute |
| Final: logits GEMM + **Sampling** (top-k/top-p/temp) | GEMM + Sampling | memory-bound | per-step at decode |

Roughly *8 GEMMs* + attention + several memory-bound epilogues (norms,
activations, residuals) per layer — the compute-bound/memory-bound *pair* the
orchestrator's tier-1 complementarity co-scheduling targets.

### 2.3 Use cases

- Fuse activation/bias/residual into GEMM epilogues (fewer launches); co-run the
  unavoidable memory-bound norms with a compute-bound GEMM from another request.

## 3. Two phases, opposite bottlenecks

### 3.1 Requirement

Model prefill vs decode explicitly; they schedule differently.

### 3.2 Mechanism

- **Prefill** processes the whole prompt in parallel → large GEMMs + full
  attention → **compute-bound**, high arithmetic intensity, one shot per request.
- **Decode** generates one token per step, reading the entire KV cache each step →
  tiny GEMMs (batch×1) + KV-gather attention → **memory-bound**, latency-bound,
  repeated hundreds of times.
- Consequences the scheduler must exploit:
  - **Co-run** (Sarathi chunked prefill + piggybacked decode) to fill decode's
    idle compute with prefill work — one batch, no stalls.
  - Or **disaggregate** (DistServe/Splitwise): prefill pool (compute-provisioned)
    feeds KV cache to a decode pool (memory/bandwidth-provisioned), removing
    prefill-decode interference on tail latency.

### 3.3 Use cases

- A long-prompt request no longer stalls in-flight decodes: its prefill is
  chunked and interleaved; p99 decode latency holds.

## 4. KV-cache residency (the scarce resource)

### 4.1 Requirement

Manage the KV cache so batch size (and utilization) is maximized without
out-of-memory or fragmentation.

### 4.2 Mechanism

- **Paged KV cache** (PagedAttention): store each sequence's KV in fixed-size
  pages, non-contiguous, allocated on demand — near-zero fragmentation, high
  batch occupancy, and **prefix sharing** (shared system prompts share pages).
- **Eviction/offload** under pressure: recompute or swap cold sequences' KV to
  host memory (ties to the orchestrator's residency manager, mirroring the gfx
  streaming residency in [`xpu-gfx-streaming-geometry`](xpu-gfx-streaming-geometry-spec.md)).
- **Block-sparse / composable formats** (FlashInfer) for long-context and
  paged attention kernels.

### 4.3 Use cases

- Doubling effective batch size at fixed memory by paging + prefix-sharing common
  prompts → higher GPU utilization.

## 5. Batching, phase scheduling, and multiplexing

### 5.1 Requirement

Turn a continuous stream of requests into GPU-saturating batches under latency
SLOs.

### 5.2 Mechanism

- **Continuous (iteration-level) batching** (Orca): admit/retire requests every
  decode iteration; selective batching handles ragged sequence lengths.
- **Chunked prefill / stall-free batch** (Sarathi): bound each iteration's work so
  a big prefill never blocks decodes; co-schedule the two phases in one batch.
- **Disaggregation** (DistServe, Splitwise): separate prefill and decode
  executors; schedule KV-cache handoff between them.
- **Statistical multiplexing** (AlpaServe, from the sibling corpus): spread bursty
  requests across replicas to cut tail latency.
- **Speculative decoding** (Leviathan): a draft model proposes k tokens; the
  target verifies them in one batched (compute-bound) pass — reshapes decode's
  schedule and raises tokens/step.
- **Admission + SLO ordering**: prioritize by deadline; the interference model
  guards co-scheduling of SLO tenants.

### 5.3 Use cases

- Mixed short/long requests share one engine at high utilization with bounded
  p99, because batching is iteration-level and prefills are chunked.

## 6. Parallelism → collectives to schedule

### 6.1 Requirement

Multi-GPU LLMs inject **communication kernels** that serialize with compute unless
overlapped.

### 6.2 Mechanism

- **Tensor parallel** (Megatron): each attention/MLP block ends in an **all-reduce**
  — the Communication family. The orchestrator overlaps it with the next GEMM
  (compute) where dependencies allow (tier-1 co-run: comm is
  interconnect-bound, GEMM is compute-bound).
- **Pipeline parallel**: layer stages across GPUs; schedule micro-batches to keep
  the pipeline full (bubble minimization).
- **Expert parallel (MoE)**: router → **all-to-all** dispatch → expert GEMMs →
  all-to-all combine; dynamic, load-imbalanced — the scheduler must handle
  variable per-expert batch sizes and overlap the all-to-all with expert compute.

### 6.3 Use cases

- Overlap the tensor-parallel all-reduce with independent compute so the
  interconnect stall doesn't idle the matrix cores.

## 7. Mapping onto the orchestrator

- LLM inference is the flagship **`ML_INFER`** class: objective = latency SLO at
  max throughput; default = tier-1 co-run under admission + adaptive batching,
  with tier-2 partition (per-model MPS/MIG) and tier-3 preemption for priority
  requests.
- The orchestrator's levers used here: **complementarity co-scheduling**
  (prefill-compute × decode-memory; comm × compute), **residency management**
  (paged KV cache), **admission + interference model** (SLO protection),
  **statistical multiplexing** (bursts). Disaggregation is a placement policy the
  orchestrator selects when prefill/decode interference dominates.
- Training (`ML_TRAIN`) reuses the same kernels forward + backward, adds gradient
  and optimizer kernels and gradient all-reduce; objective shifts to throughput +
  fairness (capacity harvesting, iteration-granularity switching).

## 8. Dependencies / risks

1. Requires foundational kernels currently missing from the library:
   **attention (paged/flash), softmax/normalization, fused activation epilogues,
   sampling/top-k, gather/scatter (routing), and collectives (all-reduce,
   all-to-all)** — these lead the ML side of the kernel backlog.
2. KV-cache paging needs a residency manager shared with the gfx streaming path.
3. Collective overlap depends on backend async/copy-queue support (tiered
   degradation).
4. MoE load imbalance stresses the scheduler's dynamic batching + interference
   model.

## 9. References

Corpus + markers in [`research/llm-serving/papers/`](../research/llm-serving/papers/):
PagedAttention/vLLM, FlashAttention (1/2), Orca (continuous batching),
Sarathi-Serve (chunked prefill), DistServe & Splitwise (prefill/decode
disaggregation), FlashInfer (kernel families), speculative decoding, Megatron
(tensor parallel), Switch/MoE, and an LLM-serving survey. Sibling specs:
[`xpu-kernel-scheduling`](xpu-kernel-scheduling-spec.md),
[`xpu-gfx-streaming-geometry`](xpu-gfx-streaming-geometry-spec.md); shared
serving references (Clockwork, Nexus, AlpaServe, iGniter) live in
`research/xpu-scheduling/papers/`.
