# MoE expert cache — mmap-resident quantized experts with a learned
# admission, eviction and prefetch policy

**Status: DRAFT, 2026-08-21.** Not approved. Open questions in §10.

## 1. Definition

### 1.1 Purpose
A mixture-of-experts model is far larger than the working set any one token
needs. Top-k routing touches k of N experts per MoE layer per token, so the
bytes a decode step actually reads are a small fraction of the checkpoint.
This capability keeps expert weights in their **packed quantized form**,
backed by the memory-mapped checkpoint, and manages residency at
**expert granularity** — admitting, evicting and prefetching whole experts
rather than whole models.

### 1.2 Problem
Materializing every expert as f32 is impossible for models of interest and
wasteful even when it fits: decode is memory-bandwidth bound, so the bytes
moved per token set the throughput ceiling. Dequantizing at load costs the
accuracy of quantization AND the bandwidth of f32. Loading every expert
costs residency proportional to the model rather than to the workload.

### 1.3 Scope
Expert-granular residency over a mapped, quantized checkpoint; a baseline
recency policy; a learned policy that improves on it; and the measurement
that decides whether the learned policy is worth its cost.

### 1.4 Non-goals
- Training or fine-tuning the LLM. The routing network is read, never changed.
- Designing the routing algorithm. Routing is an input to this system.
- Changing model outputs. See §7 — the cache is latency-and-memory only.
- MoE architecture support in the model itself. That is a prerequisite,
  tracked separately; this spec assumes routed experts exist.
- GPU-resident experts in v1 (§10.4).

### 1.5 Dependencies
- Packed quantized weights on the forward path (Q8_0 today, block-direct
  K-quants next) — without it there is nothing compact to keep resident.
- A `Storage` that can be backed by foreign (mapped) memory, non-owning.
- MoE support in the model: routed FFN experts and a router.

## 2. Expert addressing

### 2.1 Requirements
An expert must be nameable, locatable and measurable without decoding it.

- **2.1.1** When a GGUF checkpoint is opened, each expert's byte range is
  computable from the tensor directory (name → offset → length) without
  reading tensor data.
- **2.1.2** When experts are stored as one stacked tensor per layer
  (`blk.N.ffn_*_exps.weight`, leading dimension `n_expert`), the byte range
  of expert `e` is derived by even subdivision, and a remainder is an error
  naming the tensor.
- **2.1.3** When experts are stored as one tensor per expert, each maps to
  its own directory entry.
- **2.1.4** When an expert's range is requested, its ggml block type and
  element count are reported with it, so a consumer can size and decode it.
- **2.1.5** When a checkpoint is not MoE, the addressing layer reports zero
  experts rather than failing.

## 3. Residency

### 3.1 Requirements
Residency is capacity-bounded, expressed in bytes, and independent of the
number of experts.

- **3.1.1** When an expert is required by routing and is resident, it is
  used without I/O.
- **3.1.2** When an expert is required and is not resident, it is admitted
  before use, evicting as needed to stay within the byte budget.
- **3.1.3** When admitting an expert would exceed the budget, victims are
  evicted until it fits, and an expert in use by the current step is never
  a victim.
- **3.1.4** When the budget cannot fit the experts one token needs, the
  configuration is rejected at load with the required minimum.
- **3.1.5** When an expert is evicted, only its residency is dropped; its
  bytes remain available from the mapping.
- **3.1.6** When the mapping itself is the residency mechanism, eviction is
  advisory (the page cache owns the pages) and the accounting reports both
  the advisory intent and the observed resident size.
- **3.1.7** When experts are shared across layers or tied, they are
  reference-counted so eviction cannot pull an expert still in use.

### 3.2 Baseline policy
- **3.2.1** When no learned policy is available, eviction is least-recently-used
  over experts, which is the reference every other policy is measured against.
- **3.2.2** When a policy is selected, it is named in the run report, so a
  measurement is never attributed to the wrong policy.

## 4. Prefetch

- **4.1** When the router's decision for a step is known, the experts it
  selects are made resident before the FFN needs them.
- **4.2** When a prefetch is issued for an expert already resident, it is a
  no-op and is counted separately from a fault.
- **4.3** When a prefetch is wrong, the cost is bounded to the bytes touched
  and the step still produces the correct result (§7).
- **4.4** When prefetching would evict an expert the current step still
  needs, the prefetch is refused rather than the expert evicted.
- **4.5** When the backing store is a mapping, prefetch is a page-residency
  hint over the expert's byte range, not a copy.

## 5. The learned policy

### 5.1 Rationale
Recency is a weak model of MoE routing. Routing is skewed and correlated:
some experts are hot for a whole conversation, some co-activate, and the
distribution shifts with the workload. Those are learnable regularities,
and the signal — which experts were requested, in order — is produced by
inference itself, free and continuously.

### 5.2 Requirements
- **5.2.1** When a step's routing is observed, it is recorded as training
  signal without blocking the step.
- **5.2.2** When the policy is asked for an eviction victim, it returns a
  ranking over resident experts, and the cache evicts from the bottom.
- **5.2.3** When the policy is asked what to prefetch, it returns experts
  predicted for the next step(s), bounded by a configured budget.
- **5.2.4** When the policy has seen fewer than a configured number of
  observations, the baseline policy (§3.2.1) is used instead, so a cold
  model is never worse than LRU.
- **5.2.5** When the policy's measured hit rate falls below the baseline's
  over a trailing window, it is disabled for the remainder of the run and
  the fallback is reported.
- **5.2.6** When inference is running, a policy update costs a bounded,
  measured amount of time per step, and that cost is reported alongside
  the hit rate it buys.
- **5.2.7** When the learned policy is disabled by configuration, no model
  is loaded and no training signal is recorded.

### 5.3 Why SPELA
SPELA(O) trains with local per-layer losses in a single forward sweep, with
no backward pass and no stored activations. Three properties matter here:
its update cost is a forward pass, so it fits in an inference hot path; it
adapts online to drift, which is what a changing workload is; and
head-only adaptation lets a stable backbone carry general routing structure
while a small head tracks the current session.

- **5.3.1** When the policy updates, it does so forward-only, with no
  backward pass.
- **5.3.2** When the workload's routing distribution drifts, the policy
  adapts within a bounded number of observations.
- **5.3.3** When adaptation is restricted to the head, the backbone weights
  are unchanged.

### 5.4 Features and target
- **5.4.1** When a prediction is made, its inputs are drawn only from
  information available before the FFN executes: recent per-expert
  request history, current-step router scores, layer index, position, and
  per-expert recency and frequency.
- **5.4.2** When a feature would require the answer being predicted, it is
  not used.
- **5.4.3** When the target is expressed, it is the set of experts required
  at the next step, so prediction and eviction share one model.

## 6. Per-model weights

### 6.1 Requirements
Routing structure is a property of a specific model. Weights learned on one
must never be applied to another.

- **6.1.1** When a policy is created, its weights belong to exactly one LLM,
  identified by the checkpoint's identity.
- **6.1.2** When a model is opened and weights exist for its identity, they
  are loaded; otherwise a fresh policy starts cold (§5.2.4).
- **6.1.3** When a model's identity does not match the weights on disk, the
  weights are refused, not adapted.
- **6.1.4** When two models run in one process, each has its own policy
  instance and neither observes the other's routing.
- **6.1.5** When a run ends, updated weights are persisted for that identity.
- **6.1.6** When persisting fails, inference is unaffected and the failure
  is reported.
- **6.1.7** When weights are stored, they are versioned by feature-schema,
  and a mismatched schema starts cold rather than misinterpreting weights.
- **6.1.8** When the same model is served by several processes, weight
  persistence does not corrupt under concurrent writes.

## 7. Correctness

### 7.1 Requirements
The cache is a performance mechanism. It has no licence to change results.

- **7.1.1** When the same prompt is run twice with different cache states,
  budgets, or policies, the outputs are identical.
- **7.1.2** When a prediction is wrong, the only consequences are latency
  and bytes moved.
- **7.1.3** When an expert is admitted, the bytes used are the checkpoint's
  bytes, and a mismatch against the directory's length is an error.
- **7.1.4** When the byte budget is set to its minimum, results match an
  unbounded run.

## 8. Observability

- **8.1** When a run completes, it reports per-expert-cache: hit rate, fault
  count, admissions, evictions, prefetches issued, prefetches unused, bytes
  admitted, and peak resident bytes.
- **8.2** When the learned policy is active, the report adds its
  observation count, update time per step, and its hit rate against the
  baseline's on the same trace.
- **8.3** When a routing trace is captured, it can be replayed offline
  against any policy without running the model.
- **8.4** When a policy is compared to another, the comparison is on the
  same trace and the same budget.

## 9. Acceptance

- **9.1** When the learned policy is evaluated on a held-out routing trace,
  it must beat LRU on hit rate at equal budget, or it does not ship.
- **9.2** When the learned policy is active, end-to-end decode throughput
  must be no worse than LRU's — a hit-rate win consumed entirely by update
  cost is not a win.
- **9.3** When resident bytes are capped below the model size, output is
  unchanged (§7.1.1) and the run completes.
- **9.4** When the policy is cold, throughput matches the LRU baseline
  within noise.

## 10. Open questions

- **10.1 Where does the policy execute?** SPELA today is PyTorch, and the
  engine is cajeta. Options: implement on `dev.cajeta.ml` and run in
  process; run offline and ship read-only weights, losing online adaptation;
  or a sidecar. In-process on cajeta-ml is the only option that satisfies
  §5.2.1 and §5.3.2, and it is the largest. **Recommendation: in-process on
  cajeta-ml, with an offline trace-replay harness (§8.3) used to develop the
  model before it is wired live.**
- **10.2 What is "model identity" (§6.1.1)?** GGUF metadata name plus
  architecture and expert counts is human-meaningful but collides across
  quantizations; a hash of the tensor directory is exact but opaque.
  **Recommendation: hash of the tensor directory, with the human name
  stored alongside for display.**
- **10.3 Does eviction mean anything under mmap?** If residency is the page
  cache, "evict" is advisory and the OS may keep or drop pages regardless
  (§3.1.6). The alternative is an explicit buffer pool that copies expert
  bytes out of the mapping, giving exact control at the cost of a copy and
  double residency. **Recommendation: measure both; start advisory.**
- **10.4 GPU experts.** Device residency has a hard budget and an explicit
  transfer, which suits an exact policy better than mmap does — but adds a
  transfer to every admission. Deferred, and the §5 interface should not
  assume host memory.
- **10.5 Is per-session adaptation worth it over a per-model prior?** If
  routing skew is stable per model, an offline-trained prior may capture
  most of the win with none of the hot-path cost. §9.1's trace comparison
  should include "prior only, no online updates" as an arm.
- **10.6 Trace privacy.** A routing trace is derived from user prompts.
  Whether traces may be persisted, and under what retention, is a policy
  question before §8.3 ships anything to disk.
