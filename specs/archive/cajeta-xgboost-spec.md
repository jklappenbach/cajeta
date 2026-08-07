# cajeta-xgboost — nucleo-native XGBoost, bit-exact with the reference

## 1. Definition

### 1.1 Purpose
A gradient-boosted decision tree (GBDT) library written natively in cajeta
(`cajeta-xgboost`, a public `.cja` library like `cajeta-codec`), reproducing
XGBoost's `hist` algorithm. The near-term differentiator is hardware: XGBoost's
GPU path is CUDA-first and does not run on AMD APUs (Strix Halo), so a
cajeta-native GBDT that compiles to that GPU is a capability the Python
ecosystem cannot deliver there. NVIDIA is the GPU **proving ground** (where the
kernels can be cross-checked against XGBoost's own `gpu_hist`); Strix Halo is the
**payoff**.

### 1.2 The North Star — bit-exact parity
The library's correctness bar is **bit-exact agreement with reference XGBoost**:
given identical inputs and hyperparameters, cajeta produces the identical trees
(structure + leaf weights) and identical predictions, to the bit. This is what
"spot on" means and it governs every design choice — numeric types, accumulation
order, tie-breaking, and the determinism knobs are all fixed to make
bit-exactness reachable, not merely tight-tolerance. XGBoost `hist` is
deterministic (recent versions made even multi-threaded histogram reduction
deterministic), so this is a design target, not a fantasy.

**Why it must be bit-exact — the motivation.** The failure that makes a GBDT
library worthless on a poorly-supported platform is that its numbers *don't match
the reference the rest of the team runs*. A model scored on AMD that disagrees
with the same model on NVIDIA is untrustworthy. So bit-exactness has two faces,
both required:
- **Reference parity**: cajeta matches reference XGBoost (§1.6) to the bit.
- **Cross-backend identity**: cajeta's own result is **bit-identical across every
  backend it runs on** — cajeta-CPU == cajeta-AMD == cajeta-NVIDIA. This is the
  actual fix for "AMD didn't match NVIDIA": the answer is the same everywhere,
  and that answer is the reference's.

Bit-exactness is layered so a failure is diagnosable and there is always a
provable floor:
- **Structural parity** (the floor): every node's split feature, bin threshold,
  and default (missing) direction identical; identical tree shape.
- **Weight parity**: every leaf weight bit-identical.
- **Prediction parity**: every holdout margin and probability bit-identical.

### 1.3 Scope (v1)
- Tree method: **`hist`** only (the modern default and the GPU-mappable one).
- Objectives: **`reg:squarederror`**, **`binary:logistic`**, **`multi:softprob`
  / `multi:softmax`** (one tree group per class per round).
- Hyperparameters: `eta`, `max_depth`, `min_child_weight`, `lambda` (L2),
  `gamma` (min split loss), `max_bin`, `base_score`, `num_boost_round`,
  `grow_policy` (`depthwise` — `lossguide` is a stretch), and the
  missing-value **default direction**.
- Feature importance (gain / cover / weight) and model round-trip
  (train → serialize → load → predict identically).

### 1.4 Non-goals (v1, spec-sanctioned deferrals)
`exact` / `approx` tree methods; `gblinear`; DART; native categorical splits;
distributed / external-memory training; the full sklearn API surface (v1 gives
a focused `fit`/`predict` + core params); subsampling / column-sampling **inside
the bit-exact config** (see 6.4 — RNG parity is a separate, later concern).

### 1.5 Systems
`cajeta.nucleo.frame` (Table/Column ingestion), `cajeta.nucleo.column`
(Arrow-laid-out buffers), `cajeta.xpu` (the GPU phase: `IntegerAtomics`,
`WavePrefixScan`, `WaveReductions`, `KernelStream`), `cajeta-unit` (tests). A
Python harness (`xgboost` + `scikit-learn` + `numpy`) generates golden fixtures
**on an NVIDIA GPU** (§1.6), since the reference is the CUDA algorithm.

### 1.6 The reference — the NVIDIA GPU algorithm
The parity target is **XGBoost's GPU histogram algorithm** (`tree_method="hist",
device="cuda"`, formerly `gpu_hist`) in its **deterministic** mode, run on
NVIDIA, at a **pinned XGBoost version**. This is the "provided on NVIDIA"
reference users compare against — and it does **not** bit-match XGBoost's CPU
`hist` (different sketch, histogram layout, and reduction), so choosing it is
load-bearing: cajeta reproduces the *GPU* algorithm's numerics, not the CPU
path's. cajeta-CPU is therefore "the NVIDIA algorithm, computed portably on the
CPU," verified against GPU-generated golden fixtures — the same algorithm cajeta
later runs on its own GPU backends (§8). One algorithm, everywhere.

### 1.7 Determinism is the API contract
Bit-exact, deterministic execution is the **default and the guarantee**. Any mode
that cannot be bit-exact — a faster non-deterministic reduction, or (until RNG
parity lands) subsampling / column-sampling — is **explicitly labeled in the
API** and off by default, so a user only ever forfeits determinism knowingly.
- **1.7.1** As a user, when I train with defaults, then execution is deterministic
  and bit-exact to the reference; I never lose reproducibility silently.
- **1.7.2** As a user, when I opt into a non-deterministic mode, then the API
  named it as such (a `nondeterministic`/`fast` flag or a distinctly-named
  entry point) and the docs state the parity it forfeits.
- **1.7.3** As a user, when I read a model's metadata, then whether it was trained
  in a deterministic mode is recorded, so a result's reproducibility is auditable.

---

## 2. Data ingestion & quantile binning *(the `GHistIndexMatrix`)*

The `hist` method pre-bins every feature into at most `max_bin` bins via a
weighted quantile sketch, yielding per-instance bin indices plus the cut points.
Binning is the first parity surface; to isolate it, the parity harness feeds
identical bin assignments to both sides first (§7.1), then proves cajeta's own
sketch reproduces XGBoost's cut points (§7.2).

- **2.1** As a user, when I build a training matrix from a `Table`/`Column` set
  or a dense float array, then each feature is quantile-binned into ≤ `max_bin`
  bins and I get a compact per-instance bin-index matrix + the cut points.
- **2.2** As a parity author, when I bin a feature with cajeta and with XGBoost
  under the same `max_bin`, then the **cut points are identical** (same count,
  same edges to the bit), so downstream histograms are comparable.
- **2.3** As a user, when a feature has missing values, then missing is a
  distinct bin-absence (not a sentinel value), routed at split time by the
  node's learned default direction (§4.4) — never imputed.
- **2.4** As a user, when a feature has fewer distinct values than `max_bin`,
  then bins collapse to the distinct values exactly as XGBoost does (no empty
  interior bins that shift edges).

## 3. Objectives & the boosting loop

Each round computes per-instance gradient + hessian from the objective on the
current margins, grows a tree (or one per class), and adds it scaled by `eta`.

- **3.1** As a user, when I train `reg:squarederror`, then per-instance
  `grad = pred − label`, `hess = 1`, and the margin update matches XGBoost.
- **3.2** As a user, when I train `binary:logistic`, then `p = sigmoid(margin)`,
  `grad = p − label`, `hess = max(p·(1−p), eps)` with XGBoost's exact `eps`, and
  probabilities match to the bit.
- **3.3** As a user, when I train `multi:softprob`/`softmax` with K classes, then
  each round grows K trees over the softmax gradients/hessians, and per-class
  margins + the arg-max/probability outputs match XGBoost.
- **3.4** As a user, when I set `base_score`, then the initial margin is exactly
  XGBoost's (including its objective-specific transform of `base_score`).
- **3.5** As a user, when I run `num_boost_round` rounds, then the round-by-round
  margin trajectory is bit-identical to XGBoost's (a per-round parity check, not
  just the final model).

## 4. The `hist` tree builder

Per node: build per-feature (grad, hess) histograms over the node's instances
(with the sibling-subtraction trick — a child's histogram is the parent's minus
its sibling's), scan bins to find the maximum-gain split, apply stopping rules,
recurse.

- **4.1** As a builder, when I compute a node histogram, then for every
  (feature, bin) the accumulated (grad, hess) equals XGBoost's to the bit
  (§6 fixes the accumulation type and order that make this hold).
- **4.2** As a builder, when I use sibling subtraction, then the subtracted
  histogram is bit-identical to a freshly-built one (subtraction must not drift).
- **4.3** As a builder, when I score splits, then `gain = 0.5·[GL²/(HL+λ) +
  GR²/(HR+λ) − G²/(H+λ)] − γ` computed in XGBoost's precision, and the chosen
  split (feature, bin, direction) matches — including **tie-breaking** on equal
  gain (the reference's first-wins / feature-order rule, replicated exactly).
- **4.4** As a builder, when a feature has missing values, then both default
  directions are scored and the higher-gain one is chosen, matching XGBoost's
  default-direction decision at every node.
- **4.5** As a builder, when a candidate split violates `min_child_weight`
  (child hessian sum) or yields gain ≤ 0 after `gamma`, then it is rejected
  exactly as XGBoost rejects it (the node becomes a leaf identically).
- **4.6** As a builder, when I reach `max_depth` or no positive-gain split
  exists, then the node is a leaf with weight `w = −G/(H+λ)` computed and
  rounded (double→`float`) identically to XGBoost.

## 5. Prediction & model round-trip

- **5.1** As a user, when I predict, then each instance walks every tree by bin
  threshold + default direction, the leaf `float` values sum (in XGBoost's
  order) into a margin, and the objective's inverse link produces the output —
  all bit-identical.
- **5.2** As a user, when I serialize a trained model and load it back, then
  predictions are bit-identical to the in-memory model (stable round-trip).
- **5.3** As a user, when I request feature importance (gain/cover/weight), then
  the values match XGBoost's for the same model.

## 6. The bit-exact numeric contract *(the crux)*

Bit-exactness across two implementations requires replicating XGBoost's exact
arithmetic, not just its formulas.

- **6.1** As a parity author, I require the **accumulation types** to match:
  per-instance gradient/hessian in `float`, histogram bins accumulated in
  `double` (XGBoost's `GradientPairPrecise`), gain/weight in `double`, leaf
  stored as `float`, prediction margin accumulated in the reference's type.
- **6.2** As a parity author, I require the **reference GPU reduction order**
  to be reproduced: the deterministic GPU histogram sums (grad, hess) per bin in
  a defined order/precision, and cajeta reproduces those exact sums on **every**
  backend — on CPU by replicating that reduction portably, on GPU (§8) by the
  same deterministic reduction — so non-associative `double` addition yields
  identical bits everywhere.
- **6.3** As a parity author, I require **tie-breaking and rounding** rules
  (equal-gain split choice, double→float leaf rounding mode) to match the
  reference exactly.
- **6.4** As a parity author, I require a **pinned reference config** for the
  bit-exact bar: the reference XGBoost **version** (§1.6), GPU **deterministic**
  mode, fixed `seed`, `subsample=1`, `colsample_*=1`, fixed `max_bin`.
  Subsampling / column-sampling parity (replicating XGBoost's PRNG) is a later
  unit; until it lands, those knobs are a **labeled non-deterministic mode**
  (§1.7), not part of the bit-exact bar.
- **6.5** As a parity author, when a config *outside* the deterministic set is
  used, then it is an **API-labeled non-deterministic mode** (§1.7) — the library
  still trains correctly and holds structural + tight-tolerance parity, and never
  silently claims reproducibility it doesn't have. Bit-exactness is claimed only
  where 6.4 holds.

## 7. The parity harness *(extra, extra care)*

A Python script trains reference XGBoost on a **large** dataset under the pinned
config and dumps golden artifacts (cut points, per-round margins, model JSON with
every tree, holdout predictions). cajeta's tests load these and assert
bit-exactness, phased so failures localize.

- **7.1** *(binned-boundary first)* As a parity author, when I feed cajeta and
  XGBoost **identical bin assignments**, then cajeta's trees + predictions are
  bit-identical — proving the tree builder in isolation from the sketch.
- **7.2** *(sketch parity)* As a parity author, when cajeta bins the raw features
  itself, then its cut points equal XGBoost's (§2.2), closing the raw→prediction
  loop.
- **7.3** *(scale)* As a parity author, the dataset is large enough to exercise
  real tree depth and bin occupancy — a fixed-seed synthetic (≥100k rows, ≥50
  features, controlled missing-value fraction) **plus** a real public benchmark
  (e.g. covertype / Higgs subset) — not a toy that hides divergence.
- **7.4** *(per-round, per-tree)* As a parity author, parity is asserted at each
  boosting round and each tree, so the **first** divergent node is pinpointed,
  not just a final-model mismatch.
- **7.5** *(regression guard)* As a maintainer, the golden fixtures are committed
  and the parity suite runs in CI, so any drift from bit-exactness fails the
  build with the exact diverging (round, tree, node).
- **7.6** *(objective coverage)* As a parity author, the harness covers all v1
  objectives (regression, binary, multiclass) with their own fixtures.

## 8. The GPU phase — NVIDIA (the reference) → Strix Halo (the payoff)

Deferred until CPU bit-exactness is rock-solid ("let me know when we're ready").
Because the reference *is* the NVIDIA GPU algorithm (§1.6), the GPU phase closes
the loop: cajeta's own GPU kernels must produce results **bit-identical to
cajeta-CPU/AMD *and* to the XGBoost GPU reference**.

- **8.1** As a GPU author, when I build node histograms on the GPU
  (gradient/hessian scatter via `IntegerAtomics`, bin scan via `WavePrefixScan`,
  gain via `WaveReductions`) in the deterministic reduction of §6.2, then the
  resulting trees are bit-identical to cajeta's CPU path.
- **8.2** As a GPU author, when I run cajeta on **NVIDIA**, then its output is
  bit-identical to the XGBoost GPU reference (§1.6) — the parity target met on
  the reference's own hardware — and identical to cajeta-CPU/AMD (cross-backend
  identity, §1.2).
- **8.3** As a GPU author, when the same code runs on **Strix Halo (AMD APU)**,
  then it produces those same bits — a GBDT matching the NVIDIA reference on
  hardware the Python/CUDA stack cannot run at all. The payoff.

---

## 9. Acceptance criteria (spec-level)

- A GBDT trained under the §6.4 deterministic config is **bit-exact with the
  NVIDIA GPU reference** (§1.6): identical trees (structure + weights) and
  identical holdout predictions, for `reg:squarederror`, `binary:logistic`, and
  multiclass, on a large dataset — asserted per round / per tree / per node (§7).
- **Cross-backend identity**: cajeta's result is bit-identical across every
  backend (CPU / AMD / NVIDIA) — the same bits everywhere (§1.2, §8).
- The tree builder is proven correct in isolation (identical-bins boundary, §7.1)
  before sketch parity (§7.2) closes the raw→prediction loop.
- **Determinism is the default and is labeled**: non-deterministic modes are
  opt-in, named in the API, and record their mode in model metadata (§1.7);
  outside the deterministic config, parity is structural + tight-tolerance and
  never a silent reproducibility claim (§6.5).
- Feature importance and model serialization round-trip match the reference
  (§5.2–5.3).
- The parity suite runs in CI against committed golden fixtures; drift fails with
  the exact diverging location (§7.5).
- (Phase 2) cajeta's own GPU kernels produce output bit-identical to the XGBoost
  GPU reference on NVIDIA (§8.2) and to cajeta-CPU/AMD, then run on Strix Halo
  (§8.3).

## 10. Open questions (resolve at plan time)

- **Reference version + deterministic-GPU availability**: pin one XGBoost
  version whose GPU `hist` has a **deterministic** mode, and record it — the
  bit-exact target is version-specific. (Older `gpu_hist` used non-deterministic
  atomic adds; determinism must be confirmed for the pinned version.)
- **Fixture-generation hardware**: golden fixtures require an **NVIDIA GPU +
  CUDA + xgboost-gpu**. Confirm access (the same hardware the §8.2 NVIDIA
  validation needs) and whether fixtures are generated once + committed, or
  regenerated in CI.
- **GPU sketch fidelity**: XGBoost's GPU quantile sketch (device sketch /
  `WQSketch`) edge cases (duplicates, sparse/missing) — replicate exactly, or
  feed pre-binned data in v1 and treat full-sketch parity as its own unit?
  *(Lean: builder parity first on identical bins; sketch parity as a dedicated
  unit.)*
- **`double` semantics across cajeta backends** for §6.2/§8.1 — confirm the CPU
  and GPU (NVIDIA + AMD) paths all give IEEE-754 `double` with the same rounding,
  so the reproduced reduction is bit-identical everywhere.
- **Dataset licensing** for the committed real-benchmark fixture (covertype/Higgs
  redistribution vs. a script that fetches + a committed synthetic).
- **base_score default** handling for the pinned version (older constant vs.
  newer fitted `base_score`) — pin explicitly.
