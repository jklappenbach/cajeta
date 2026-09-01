# MoE expert cache — tiered expert residency over a mapped quantized
# checkpoint, with a measured path to a learned policy

**Status: DRAFT (rewritten 2026-09-01; original draft 2026-08-21).**
The rewrite grounds the spec in what cajeta-llm units 26–29 shipped and
measured, reverses the original's host-first orientation (§10.4 deferred
device residency; the measurements made it the whole point), and re-tiers
the work: T0 shipped, T1 is this spec's deliverable, T2 is specified but
gated on T1's records. Four structural decisions resolved with Julian
2026-09-01: T1 carries the device decode dispatch; T1 has NO eviction
(windowed fallback past budget); the budget is an EngineOptions knob with
a measured-safe default; this document owns the mechanism (llm-spec 15.19
points here).

## 1. Definition

### 1.1 Purpose
A mixture-of-experts model is far larger than the working set any one
token needs, but the working set of a *prefill* — and of a long serving
session — converges on most of the model. Top-k routing touches k of N
experts per MoE layer per token; a 512-row prefill batch touches
essentially all N in every layer (512 rows × 8 picks over 128 on the
Qwen3-30B witness). This capability keeps expert weights resident **on
the device, in the working form the dispatching kernel reads**, admitted
lazily on first touch and bounded by an explicit byte budget, so an
expert crosses the bus once per process instead of once per layer per
batch.

### 1.2 Problem — measured, 2026-09-01
After cajeta-llm unit 29 (batched attention + grouped expert dispatch),
Qwen3-Coder-30B-A3B Q4_K_M on the Strix Halo box measures:

| arm | prefill @512 | decode |
|---|---|---|
| cajeta-llm (windowed experts) | 39.0 ms/tok | 811.6 ms/tok |
| llama.cpp Vulkan (resident experts) | 0.97 ms/tok | 12.0 ms/tok |

The dominant residual on both sides is expert *non-residency*:

- **Prefill**: every layer of every batch re-reads each selected
  expert's stride from the page cache, re-uploads it into the shared
  expert window, and re-runs its repack kernels — ~16 GiB (the file's
  whole expert payload) re-crosses per prefill pass.
- **Decode**: expert mat-vecs run on the host floor because there is
  nothing on the device to dispatch against; ~800 of the 812 ms/tok is
  expert work whose bandwidth floor on this box is ~8 ms/tok.

### 1.3 The tiers
- **T0 — the OS pages (SHIPPED, llm units 26–28; llm-spec 15.19 v1).**
  GGUF loads through `MappedFile`; `ExpertBank` binds a file offset and
  reads expert strides through the page cache into reused staging. An
  expert that never fires is a page range never touched; the page cache
  is the host-side eviction policy. The load-bearing constraint carries
  forward unchanged: **nothing may eagerly materialize a full expert
  slab** — no load-time pass over all experts, host or device.
- **T1 — device-resident expert slots (THIS SPEC'S DELIVERABLE).**
  First-touch admission into per-expert device slots, an explicit byte
  budget, no eviction: past budget an expert simply keeps taking the T0
  windowed path, which is the fallback and not an error. T1 also carries
  the device DECODE dispatch: a resident expert's mat-vec runs on the
  device. Lazy-but-not-amnesiac keeps T0's letter.
- **T2 — the managed policy (SPECIFIED HERE, §5–§6; gated).** Eviction,
  router-guided prefetch, and the learned (SPELA) policy. Gated on the
  routing-utilization records (llm-spec 15.20 / llm plan unit 30)
  measuring what T1 actually does on a real oversized model — both this
  spec's original draft and llm-spec 15.19 v2 required that ordering,
  and it stands.

### 1.4 Non-goals
- Training or fine-tuning the LLM. The routing network is read, never
  changed.
- Designing the routing algorithm. Routing is an input to this system.
- Changing model outputs. See §7 — the cache is latency-and-memory only.
- Long-context attention cost. At 128k context the KV read dominates
  every MoE equally (llm-spec 15.19's envelope); that is llm unit 31's
  subject.

### 1.5 Dependencies
- llm units 26–28: expert addressing, `ExpertBank`, the grouped
  dispatch, the expert window (all shipped).
- llm unit 30 (utilization records) — prerequisite for T2 only.
- `dev.cajeta.ml` 0.10.0 (`SpelaTrainer`) — T2 only.

## 2. Expert addressing — SHIPPED, restated as the contract

Implemented by llm units 26–28 (`GgufFile.packedFileOffsetOf`,
`ExpertBank.bind`, stride subdivision); restated so a regression is a
spec violation, not a style change.

- **2.1.1** When a GGUF checkpoint is opened, each expert bank's byte
  range is computable from the tensor directory without reading tensor
  data.
- **2.1.2** When experts are stored as one stacked tensor per layer
  (`blk.N.ffn_*_exps.weight`), expert `e`'s range is derived by even
  subdivision, and a remainder is an error naming the tensor.
- **2.1.3** When a checkpoint uses the legacy one-tensor-per-expert
  layout, it is rejected naming the fix (llm-spec 15.17).
- **2.1.4** When an expert's range is requested, its ggml type and
  element count are reported with it.
- **2.1.5** When a checkpoint is not MoE, the addressing layer reports
  zero experts rather than failing.

## 3. T1 residency

### 3.1 Requirements
- **3.1.1** When an expert bank is dispatched on the device and its
  slot is resident, it is used with no host read, no upload, and no
  repack — the admission's launches are the last that expert costs.
- **3.1.2** When an expert is dispatched and not resident, and the
  budget has room, it is admitted: one read from the mapping, one
  upload, one run of its repack chain, into a slot that persists for
  the engine's life.
- **3.1.3** When admitting an expert would exceed the budget, it is NOT
  admitted and its dispatch takes the T0 windowed path. No eviction in
  T1 (resolved 2026-09-01): under budget every policy is identical, and
  past budget the un-admitted set is exactly what T2's records must
  characterize before a policy is built for it.
- **3.1.4** When a slot is resident, its content is the **working form
  the dispatching kernel reads** (post-repack; per-quant), so a hit
  skips the repack chain, not just the upload. Accounting charges the
  working form's true bytes per quant — the f16-widened routes cost
  more than the int8 routes and the budget must see that, not the
  packed size.
- **3.1.5** When the same expert serves batch GEMM and decode mat-vec,
  they share one slot; a form needed by one route and not the other is
  admitted at most once.
- **3.1.6** When residency is dropped (engine shutdown, model close),
  nothing is lost: the mapping remains the source of truth and T0
  still runs. There is no persistence of slots.
- **3.1.7** When the budget is 0, T1 is off and behavior is exactly
  T0's — the fallback path IS the pre-T1 path, kept correct by the
  same tests.

### 3.2 The budget
- **3.2.1** When the engine is configured, the residency budget is an
  `EngineOptions` byte cap, host-configurable (cabra), with a default
  derived from device-visible memory and VALIDATED BY MEASUREMENT on
  this box before it ships — on UMA hardware the budget competes with
  the page cache T0 depends on, so an unlucky default can slow the
  fallback it falls back to.
- **3.2.2** When the model's expert payload fits the budget (the 30B
  witness: ~16 GiB against 96 GiB visible), steady state is full
  residency and T1 matches an eager loader's performance without ever
  having been eager.
- **3.2.3** When it does not fit (Qwen3-235B-A22B at Q4: ~120 GiB),
  the engine still loads, still runs, and still answers — degraded to
  T0 speed for the un-admitted set, with the split visible in records
  (§8).
- **3.2.4** When accounting is reported, resident bytes, slot count,
  and per-bank admission state are readable by the host (the
  `bytesHeld`/`deviceWindowBytes` shape units 26–28 established).

### 3.3 Decode on resident experts
- **3.3.1** When decode selects an expert whose slot is resident, its
  mat-vec launches on the device against the slot; only an un-admitted
  expert's mat-vec runs on the host floor.
- **3.3.2** When a decode step mixes resident and un-admitted experts,
  each expert takes its own path and the results are combined exactly
  as the host path combines them (§7.1.1 holds per step, not just per
  run).
- **3.3.3** When all selected experts are resident, the decode step's
  expert cost approaches the device bandwidth floor (~8 ms/tok for
  3B activated at Q4 on this box's 205 GB/s), and the measured number
  is recorded against llama.cpp's on the same file (the llm-spec
  15.15 / 20.1.6 shape).

## 4. T2 — prefetch (gated on unit 30's records)

Unchanged in intent from the original draft; renumbered under T2. The
router knows a layer's selections before that layer's FFN runs, and
15.20 knows a session's expert affinity across turns — those two facts
are the prefetch seam.

- **4.1** When the router's decision for a step is known, the experts it
  selects are made resident before the FFN needs them.
- **4.2** When a prefetch targets an expert already resident, it is a
  no-op counted separately from a fault.
- **4.3** When a prefetch is wrong, the cost is bounded to the bytes
  touched and the step still produces the correct result (§7).
- **4.4** When prefetching would evict an expert the current step still
  needs, the prefetch is refused rather than the expert evicted.

## 5. T2 — the learned policy

*(Substantively as drafted 2026-08-21; §10.1/10.5 resolutions stand.
One reframe: with T1 evictionless, the policy's first decision is
ADMISSION under budget pressure — which experts deserve the resident
set — with eviction ranking joining only if T2 adds eviction at all.
§9.1's LRU baseline comparison applies to whichever decisions T2
actually makes.)*

### 5.0 Shape (resolved 2026-08-21, §10.5)
An offline-trained per-model PRIOR is shipped, and at serving only the
HEAD adapts (`freezeBackbone()`), so the backbone carries routing
structure learned across sessions while the head tracks the current one.
The prior-only arm is retained as a comparison in §9.1: if routing skew
proves stable per model, it may capture most of the win at no hot-path
cost, and that must be measured rather than assumed.

### 5.1 Rationale
Recency is a weak model of MoE routing. Routing is skewed and
correlated: some experts are hot for a whole conversation, some
co-activate, and the distribution shifts with the workload. Those are
learnable regularities, and the signal — which experts were requested,
in order — is produced by inference itself, free and continuously.

### 5.2 Requirements
- **5.2.1** When a step's routing is observed, it is recorded as
  training signal without blocking the step.
- **5.2.2** When the policy is asked which experts merit residency, it
  returns a ranking, and the cache admits (or, if T2 adds eviction,
  evicts) from that ranking.
- **5.2.3** When the policy is asked what to prefetch, it returns
  experts predicted for the next step(s), bounded by a configured
  budget.
- **5.2.4** When the policy has seen fewer than a configured number of
  observations, the baseline policy is used instead, so a cold model is
  never worse than the baseline.
- **5.2.5** When the policy's measured hit rate falls below the
  baseline's over a trailing window, it is disabled for the remainder
  of the run and the fallback is reported.
- **5.2.6** When inference is running, a policy update costs a bounded,
  measured amount of time per step, reported alongside the hit rate it
  buys.
- **5.2.7** When the learned policy is disabled by configuration, no
  model is loaded and no training signal is recorded.

### 5.3 Why SPELA
SPELA(O) trains with local per-layer losses in a single forward sweep,
no backward pass, no stored activations: its update cost is a forward
pass, so it fits an inference hot path; it adapts online to drift; and
head-only adaptation lets a stable backbone carry general routing
structure while a small head tracks the current session.

- **5.3.1** When the policy updates, it does so forward-only.
- **5.3.2** When the workload's routing distribution drifts, the policy
  adapts within a bounded number of observations.
- **5.3.3** When adaptation is restricted to the head, the backbone
  weights are unchanged.

### 5.4 Execution — in process, on cajeta-ml (resolved 2026-08-21, §10.1)
SPELA is implemented in cajeta as `dev.cajeta.ml.train.SpelaTrainer`
(`dev.cajeta.ml` 0.10.0) with the serving surface this design needs.
The Python `spela-training` repository is the reference implementation.

- **5.4.1** When routing is observed, it is recorded through
  `SpelaTrainer.observe(x, label)`, which buffers to `onlineBufferSize`
  and steps only when a step is due (§5.2.1's non-blocking property).
- **5.4.2** When the serving loop ends, `flush()` steps on a partial
  buffer.
- **5.4.3** When cold-start is evaluated, `observedCount()` is §5.2.4's
  counter.
- **5.4.4** When head-only adaptation is configured, `freezeBackbone()`
  / `setLayerTrainable(layer, trainable)` implement §5.3.3.
- **5.4.5** When a prediction's confidence is needed, `confidenceOf(x)`
  supplies it, so a low-confidence prefetch can be declined.
- **5.4.6** When labels are considered: the next step's routing IS the
  label — the labelled `observe` path is used and
  `observeUnlabeled`'s self-distillation must not be enabled.
- **5.4.7** When cajeta-llm adopts this, it takes a runtime
  `dev.cajeta.ml` dependency it does not have today.

### 5.5 Features and target
- **5.5.1** When a prediction is made, its inputs are drawn only from
  information available before the FFN executes: recent per-expert
  request history, current-step router scores, layer index, position,
  and per-expert recency and frequency.
- **5.5.2** When a feature would require the answer being predicted, it
  is not used.
- **5.5.3** When the target is expressed, it is the set of experts
  required at the next step, so prediction, admission and prefetch
  share one model.

## 6. T2 — per-model weights

*(As drafted 2026-08-21; resolutions §10.2/§10.6 stand.)*

- **6.1.1** When a policy is created, its weights belong to exactly one
  LLM, identified two-level: base identity (name, architecture, expert
  count, layer count) keys the weights; the tensor-directory hash is
  recorded as metadata.
- **6.1.2** When a model is opened and weights exist for its identity,
  they are loaded; otherwise a fresh policy starts cold (§5.2.4).
- **6.1.3** When a model's base identity does not match the weights on
  disk, the weights are refused, not adapted.
- **6.1.3.1** When the base identity matches but the directory hash does
  not, the weights are ACCEPTED and the difference noted — same model,
  different quantization; routing decisions are substantially the same.
- **6.1.4** When two models run in one process, each has its own policy
  instance and neither observes the other's routing.
- **6.1.5** When a run ends, updated weights are persisted through
  `dev.cajeta.ml.io.Checkpoints.save`/`load` over the trainer's modules.
- **6.1.6** When persisting fails, inference is unaffected and the
  failure is reported.
- **6.1.7** When weights are stored, they are versioned by
  feature-schema; a mismatched schema starts cold.
- **6.1.8** When the same model is served by several processes, weight
  persistence does not corrupt under concurrent writes.

## 7. Correctness

The cache is a performance mechanism. It has no licence to change
results.

- **7.1.1** When the same prompt is run twice with different residency
  states or budgets, the outputs are identical — including a step that
  mixes resident and windowed experts (§3.3.2).
- **7.1.2** When a T2 prediction is wrong, the only consequences are
  latency and bytes moved.
- **7.1.3** When an expert is admitted, the bytes used are the
  checkpoint's bytes, and a mismatch against the directory's length is
  an error.
- **7.1.4** When the budget is 0, results match a full-residency run
  (and the path IS the T0 path, §3.1.7).

## 8. Observability

- **8.1** When a run completes, per-cache accounting is reportable:
  admissions, admission bytes, resident bytes, hits, windowed
  (fallback) dispatches, and the hit/fallback split per layer. In T1
  this rides the diagnostics-records seam (llm-spec 11.8, 15.20; llm
  plan unit 30) — one record type, allocated only when a callback is
  registered.
- **8.2** When the learned policy is active, the report adds its
  observation count, update time per step, and its hit rate against the
  baseline's on the same trace.
- **8.3** When a routing trace is captured, it can be replayed against
  any policy without running the model. Traces are in-memory
  development artifacts or synthetic; user-derived traces are never
  persisted (§10.6).
- **8.4** When a policy is compared to another, the comparison is on
  the same trace and the same budget.

## 9. Acceptance

### 9.1 T1
- **9.1.1** When the 30B witness prefills at 512 under the default
  budget, no expert is admitted more than once (mechanism: the
  admission counter, not a wall-clock) and steady-state prefill has
  zero expert re-uploads.
- **9.1.2** When the 30B witness decodes with all selected experts
  resident, expert mat-vecs launch on the device, and the measured
  prefill and decode ms/tok are recorded against llama.cpp Vulkan on
  the same file (llm-spec 15.15's comparison shape). The number is
  recorded, not gated — the gate is the mechanism.
- **9.1.3** When the budget is set below the model's expert payload,
  the run completes, output is unchanged (§7.1.1), and records show
  the resident/windowed split (§8.1).
- **9.1.4** When the budget is 0, the suite's existing T0 tests pass
  unchanged on the same binary.
- **9.1.5** When a non-MoE model runs, nothing is admitted, no slot is
  allocated, and no accounting record is emitted.

### 9.2 T2 (unchanged from the original draft)
- **9.2.1** When the learned policy is evaluated on a held-out routing
  trace, it must beat the baseline on hit rate at equal budget, or it
  does not ship.
- **9.2.2** When the learned policy is active, end-to-end decode
  throughput must be no worse than the baseline's — a hit-rate win
  consumed by update cost is not a win.
- **9.2.3** When the policy is cold, throughput matches the baseline
  within noise.

## 10. Open questions

- **10.1 Policy execution.** RESOLVED 2026-08-21 — in process on
  `dev.cajeta.ml` (§5.4).
- **10.2 Model identity.** RESOLVED 2026-08-21 — two-level (§6.1.1).
- **10.3 Does eviction mean anything under mmap?** RESOLVED 2026-08-21
  for the host tier (explicit pool, not advisory), and MOOTED for T1
  2026-09-01: T1 has no eviction, and device slots are explicit by
  nature, so if T2 adds eviction the measurability concern is already
  satisfied.
- **10.4 GPU experts.** RESOLVED 2026-09-01 — device residency is T1,
  this spec's deliverable; the 2026-08-21 deferral is reversed by the
  unit 28/29 measurements (§1.2).
- **10.5 Per-session adaptation vs per-model prior.** RESOLVED
  2026-08-21 — prior plus head-only online (§5.0), decided by §9.2.1's
  control arm.
- **10.6 Trace privacy.** RESOLVED 2026-08-21 — weights only, never
  traces (§8.3).
- **10.7 Multi-model device budget.** OPEN — when two MoE models share
  one engine process (cabra can host several), whether the budget is
  per-model or per-process, and who arbitrates. T1 may assume one MoE
  model per process; the option must not be foreclosed.
- **10.8 Working-form residency vs packed residency.** OPEN for
  oversized models: holding the smaller PACKED form resident and
  re-running only the repack chain per dispatch would fit ~2–3.6x more
  experts per byte at the cost of repack launches per hit. T1 holds the
  working form (§3.1.4, the measured bottleneck is the full re-stage);
  records must keep enough data to evaluate the packed variant later.
