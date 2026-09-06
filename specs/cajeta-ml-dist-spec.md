# cajeta-ml-dist — distributed execution for `dev.cajeta.ml`'s estimators

## 1. Definition

### 1.1 Purpose

`dev.cajeta.ml` fits models on one node against tensors that fit in memory.
`cajeta-dqe` executes relational plans across a cluster. Nothing lets an
estimator run over a partitioned table.

`cajeta-ml-dist` is that layer: **a distributed execution strategy for
estimators that already exist**, not a second set of models.

### 1.2 The contract — one estimator, two execution strategies

This is the spec's defining constraint, and the reason it is a thin library
rather than a large one:

> **A distributed algorithm here must produce the same model as its single-node
> counterpart on the same data and seed** (§8.1), or state precisely why it
> cannot (§6.4).

Python's ecosystem split into scikit-learn and Spark MLlib — two
implementations, two APIs, two sets of bugs, and models that silently disagree.
Cajeta does not repeat that. `KMeans` is `dev.cajeta.ml`'s `KMeans`; this
library only changes *where the arithmetic happens*.

### 1.3 Scope basis

The distributed classical-ML surface as Spark MLlib defines it — principally
distributed **k-means** and **TF-IDF**, and recommendation over a partitioned
corpus. `nucleo-distributed-frame` §10.3 records the requirement.

### 1.4 Scope

The distributed-fit seam; data-parallel fitting; distributed prediction;
distributed evaluation; the correctness contract.

### 1.5 Non-goals

- **1.5.1** **New algorithms.** If it is not already in `dev.cajeta.ml`, it
  does not belong here.
- **1.5.2** **Distributed deep learning** — data/tensor/pipeline parallelism,
  collectives, sharded parameters and optimizer state. Owned by
  `research-platform-roadmap` §6.1 (`distributed-training-spec`) and §6.5
  (`auto-sharding-spec`), on top of `cajeta-ml-v3` §12. Different problem: those
  shard a *model* across devices under a collective-communication layer; this
  library shards *data* across nodes and merges sufficient statistics. Sharing a
  spec would give neither a coherent contract.
- **1.5.3** Model serving, or a model registry.
- **1.5.4** GPU-distributed execution.
- **1.5.5** Algorithms that cannot be expressed as partial-plus-merge without
  changing their result (§6.4 records them rather than approximating them).
- **1.5.6** **Distributed neural inference and serving** — KV-cache residency,
  continuous batching, prefill/decode disaggregation, speculative decoding.
  Owned by `xpu-tile-workload-profiles-spec` §3 and `xpu-tile-scheduling-spec`
  (which superseded `llm-kernel-scheduling` and `xpu-kernel-scheduling` on
  2026-09-06).

### 1.6 Package name

**`dev.cajeta.ml.dist`**, from repo `cajeta-ml-dist` by the derivation in
`datascience-platform-roadmap` §4. Two checks before it is committed, both
recorded in that spec's §5.8: whether cajeta permits a package nested under
`dev.cajeta.ml` to ship in a *different* archive, and the collision with
distributed neural training (§1.5.2), which must not land under
`dev.cajeta.ml.*`. Fallback if nesting is disallowed: `dev.cajeta.mldist`.

### 1.7 Systems

`cajeta-dqe` (partitioned tables, exchange, stage scheduling),
`dev.cajeta.ml` (the estimators, `Metrics`, `Split`), `cajeta.math.Tensor`,
`cajeta.math` distance kernels, `dev.cajeta.docs` (TF-IDF, §5),
`dev.cajeta.unit`.

---

## 2. Feature: the distributed-fit seam

- **2.1** When an estimator over a **partitioned table** is fitted, it runs
  distributed and returns **the same estimator type** a local fit returns — a
  fitted model is a fitted model, wherever it was computed.
- **2.2** When a fit runs locally on a single-partition table, the same call
  works with no network and no special casing, mirroring `nucleo-distributed-
  frame` §2.5.
- **2.3** When an estimator has no distributed strategy, that is a **clear
  error naming the estimator**, not a silent collect-to-one- node that turns a
  cluster job into an out-of-memory failure.
- **2.4** When a distributed fit is inspected, per-stage timing, rows
  processed, and bytes shuffled are visible — the same observability `cajeta-
  dqe` §6.6 provides, since a slow fit is usually a shuffle problem.
- **2.5** When the data is small enough to fit locally, collecting is
  **explicit** — never an implicit fallback.

---

## 3. Feature: the iterative pattern

Most classical estimators are iterate-until-converged, and each iteration is a
distributed aggregate. This is MapReduce's combiner, and the shape nearly
everything here reduces to.

- **3.1** When an estimator iterates, each iteration broadcasts the current
  parameters, computes **partial statistics per partition**, and merges them —
  only partials cross the network, never the data.
- **3.2** When partials merge, the merge is **associative and commutative**, so
  the result does not depend on partition count or arrival order. Without this,
  adding a node changes the model.
- **3.3** When a fit runs with a fixed seed on a fixed dataset, the result is
  **identical across different partition counts** (§8.1).
- **3.4** When floating-point summation order would otherwise vary, the
  reduction is deterministic — a tree reduction in a fixed order, not whichever
  partial arrives first.
- **3.5** When an iteration's convergence check needs a global value, it is one
  aggregate rather than a collect.

---

## 4. Feature: the estimators — clustering

- **4.1** When **k-means** over a partitioned table is fitted, each iteration
  assigns points locally and merges per-cluster sums and counts — the textbook
  distributed formulation, exact rather than approximate.
- **4.2** When k-means initializes, k-means++ runs distributed under the seed,
  since initialization determines the result and a local-only init would break
  §3.3.
- **4.3** When `inertia` is read, it is computed as a distributed aggregate.
- **4.4** When **DBSCAN** over partitions is fitted, either it is supported
  with its partition-boundary handling documented, or it is **declared
  unsupported** — density-based clustering across partition boundaries is
  genuinely hard and a wrong answer is worse than a refusal (§6.4).

---

## 5. Feature: the estimators — vectorization and linear models

- **5.1** When **TF-IDF** over a distributed corpus is fitted, document
  frequencies are one distributed aggregate and the result matches `cajeta-
  docs` §9.4 single-node exactly.
- **5.2** When the vocabulary is large, it is broadcast once per stage rather
  than per partition.
- **5.3** When **linear or logistic regression** is fitted, the sufficient
  statistics (`XᵀX`, `Xᵀy`) are distributed aggregates, so the normal-equations
  path is exact and needs one pass.
- **5.4** When the feature count makes `XᵀX` too large to broadcast, that is
  reported with the threshold named, rather than discovered as memory
  exhaustion.
- **5.5** When an iterative solver (IRLS for logistic, gradient descent) is
  fitted, §3's pattern applies and convergence is checked globally.
- **5.6** When features are standardized first, mean and variance are
  distributed aggregates, and the same scaler applies at predict time.

---

## 6. Feature: prediction, evaluation, and honest limits

- **6.1** When prediction runs over a partitioned table, the model is broadcast
  and prediction is embarrassingly parallel with no shuffle.
- **6.2** When a model is evaluated, `Metrics` are computed as distributed
  aggregates and match the single-node values.
- **6.3** When cross-validation runs, folds are formed over partitions without
  collecting, and the fold assignment is seeded and reproducible.
- **6.4** When an estimator **cannot** be distributed exactly, it is named, the
  reason is stated, and the options are explicit: run it locally on a sample,
  or use a documented approximation. **The library never silently substitutes
  an approximation for an exact algorithm.**

> §6.4 exists because the honest list matters more than the long one. Exact
> median, hierarchical clustering's full linkage, and t-SNE all resist exact
> distribution. Saying so is better than shipping something that returns a
> different answer than the same call made locally.

---

## 7. Open questions (resolve at plan time)

- **7.1** *(resolved 2026-08-01.)* **k-means and TF-IDF first** — the two with
  the cleanest partial-plus-merge formulations — then linear and logistic
  regression, whose sufficient-statistics form is exact and needs one pass.
- **7.2** *(resolved 2026-08-01 — a wrapper.)* The seam is
  `Distributed.fit(estimator, table)`, not a protocol estimators implement. This
  is what keeps `dev.cajeta.ml` ignorant of distribution and free of any
  dependency on `cajeta-dqe` — the dependency runs one way only.
- **7.3** *(resolved 2026-08-01 — no opt-out.)* §3.4's deterministic reduction
  has no escape hatch in v1. Reproducibility is this library's entire contract
  (§8.1), and an opt-out invites exactly the silent drift the contract exists to
  prevent.
- **7.4** *(resolved 2026-08-01 — yes, but sequenced.)* Tree ensembles
  distribute: a random forest is embarrassingly parallel across trees and is the
  cheapest win available here. **Blocked on `ml-trees-ensembles` landing first**,
  so it is not in the first wave.
- **7.5** *(resolved 2026-08-01 — here, later.)* Distributed hyperparameter
  search lives in this library, once §4–§5 exist. It is a different axis from
  distributing one fit — many models in parallel rather than one model across
  nodes — but it belongs to the same execution-strategy concern.

---

## 8. Acceptance criteria (spec-level)

- **8.1** **Every distributed fit produces a model identical to the single-node
  fit** on the same data and seed, asserted per estimator. This is the contract
  §1.2 exists to protect.
- **8.2** Results are **invariant to partition count** — fitting the same data
  split 2, 8, and 64 ways yields the same model (§3.3).
- **8.3** Reductions are deterministic; repeated runs on the same cluster give
  bit-identical results (§3.4).
- **8.4** An estimator without a distributed strategy fails with a clear
  message rather than collecting (§2.3).
- **8.5** No estimator is reimplemented — every distributed path calls the same
  `dev.cajeta.ml` code for its per-partition arithmetic (§1.2), verified by
  there being no second implementation in the tree.
- **8.6** Prediction over partitions requires no shuffle (§6.1), verified by
  measured bytes moved.
- **8.7** Any approximation is opt-in and named (§6.4); no call silently
  returns an approximate result.
