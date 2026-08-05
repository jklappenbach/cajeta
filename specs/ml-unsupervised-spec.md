# ml-unsupervised — clustering, mixture models, and manifold learning

## 1. Definition

### 1.1 Purpose

`dev.cajeta.ml` has exactly two unsupervised estimators: `KMeans` and `PCA`.
Five more clustering algorithms, the cluster-quality metrics, the
manifold-learning family, and the linear decompositions beyond PCA are all
absent. This spec closes that set.

### 1.2 Scope basis

The standard unsupervised surface — distance measures and scaling, partitional
and density-based clustering, mixture models, hierarchical clustering, cluster
evaluation, manifold learning, and linear decomposition — scoped against
scikit-learn's `cluster`, `mixture`, `manifold`, and `decomposition` modules,
plus scikit-learn-extra for K-medoids. *(Scope addition 2026-08-05, at
Julian's request: ICA, NMF, Factor Analysis, MDS, and the spectral manifold
family — Isomap, LLE, Laplacian eigenmaps — joined §9/§10. UMAP remains a
non-goal, §1.5.1.)*

### 1.3 Parity oracles

scikit-learn 1.9.0 for everything except K-medoids, which is
**scikit-learn-extra**. As with Surprise in `ml-recsys-spec` §1.3,
scikit-learn-extra is a small extension package and should not carry sklearn's
authority — its own docs describe it as holding algorithms that *fail*
sklearn's inclusion criteria.

### 1.4 Already supported — closes without work

`KMeans` (with `inertia`, seeded), `PCA` (with `explainedVarianceRatio`),
`StandardScaler` (standardization), `MinMaxScaler` (normalization). §3 asserts
the scaling formulas; it is expected to pass as-is.

### 1.5 Non-goals

- **1.5.1** UMAP. No consumer (the paper is in the corpus if it is ever wanted).
- **1.5.2** Spectral clustering, affinity propagation, mean-shift, OPTICS,
  BIRCH. No consumer.
- **1.5.3** Association-rule mining. A distinct problem family with its own
  algorithms; out of scope here.
- **1.5.4** Autoencoders and unsupervised neural methods — `cajeta-ml-v3`.

### 1.6 Systems

`cajeta.math.Tensor`, `cajeta.math.linalg.LinAlg` (`cholesky`, `svd`,
`slogdet` for §5; a symmetric eigensolver is an open question — §11.6),
`cajeta.math.stats.Stats`, `cajeta.math.random.Generator`,
`cajeta.math.distance` (§2, shipped in stdlib v0.17.0), `dev.cajeta.ml`
(`Estimator`/`Predictor`/`Transformer`, `KMeans`, `PCA`, scalers),
`dev.cajeta.unit`.

---

## 2. Feature: distance and similarity measures — **the third consumer**

The distance matrix is the basis of all clustering. The same kernels are
already required by two other specs.

> **Extract once.** This surface is needed by
> `ml-classification-gaps-spec` §5 (k-NN metrics),
> `ml-recsys-spec` §4.1/§7.2/§8.5 (cosine and Pearson similarity), and this
> spec. Three independent consumers is decisive: it belongs in one shared
> module, not implemented three times. See §10.1.

- **2.1** When a distance is computed, Euclidean, Manhattan, Chebyshev,
  Minkowski(p), and cosine are available under one interface.
- **2.2** When a pairwise distance matrix over `n` points is computed, the
  result is the `(n, n)` matrix the clustering algorithms consume, and its
  symmetry and zero diagonal are guaranteed.
- **2.3** When distances between two different sets are computed, the cross-
  distance matrix is available — scipy's `cdist` to §2.2's `pdist`.
- **2.4** When a vector has zero norm and I request cosine distance, the result
  is defined rather than a division by zero.
- **2.5** When a metric is added, every consumer gets it — no algorithm
  hardcodes a distance.

---

## 3. Feature: scaling — expected to pass as-is

- **3.1** When data is normalized, `y = (x − min) / (max − min)` maps to `[0, 1]`,
  matching `MinMaxScaler`.
- **3.2** When data is standardized, `y = (x − mean) / stdDev` gives zero mean and
  unit variance, matching `StandardScaler`.
- **3.3** When a feature is constant, scaling does not divide by zero — the
  degenerate case both formulas hide.
- **3.4** When unscaled data with mixed units is clustered, the documentation
  states plainly that distance-based clustering is scale-sensitive and that
  scaling is effectively mandatory: one kilometre must not weigh the same as
  one kilogram.

---

## 4. Feature: partitional clustering

- **4.1** When `KMeans` is fitted, it works as today — restated so the spec's
  acceptance covers the whole family.
- **4.2** When `KMedoids` is fitted, cluster centers are **actual data points**
  (medoids) rather than means, so the result is robust to outliers in a way
  K-means is not — the reason to reach for it.
- **4.3** When `KMedoids` with a metric is fitted, any §2 metric works,
  defaulting to Euclidean. This is a real advantage over K-means, whose mean
  update assumes Euclidean geometry.
- **4.4** When `KMedoids` is initialized, the `heuristic` strategy is available
  and the fit is seeded and reproducible.
- **4.5** When a maximum iteration count is set, it is honored and non-
  convergence is reported rather than silently accepted.

---

## 5. Feature: Gaussian mixture models

The generative counterpart to K-means: each cluster is a Gaussian, and
membership is a probability rather than a hard assignment.

- **5.1** When a `GaussianMixture` with `k` components is fitted, means,
  covariances, and mixing weights are estimated by expectation-maximization.
- **5.2** When a mixture predicts, the result is both hard assignments and **posterior
  responsibilities** per component — soft assignment is the point of the model.
- **5.3** When a covariance type is chosen, full, tied, diagonal, and spherical
  are available, since that choice is the model's main bias-variance dial.
- **5.4** When a component's covariance becomes singular, a configurable
  regularization floor is applied, and collapse is reported — EM driving a
  component onto a single point is the classic GMM failure.
- **5.5** When model selection statistics is read, AIC and BIC are available,
  so component count can be chosen rather than guessed.
- **5.6** When a mixture is fitted, initialization (K-means or random) is
  selectable and seeded, and the log-likelihood is monotonically non-decreasing
  across iterations — asserted by test, since a decrease means the EM
  implementation is wrong.
- **5.7** When EM does not converge within the iteration budget, it says so and
  `converged()` reports it.

---

## 6. Feature: hierarchical clustering

- **6.1** When agglomerative clustering is fitted, each point starts as its own
  cluster and the two closest clusters merge greedily until the requested count
  remains.
- **6.2** When a linkage is chosen, ward, complete, average, and single are
  available, with **ward** the default, matching sklearn.
- **6.3** When ward linkage with a non-Euclidean metric is used, the call is
  rejected — ward's update formula is only valid for Euclidean distance, and
  sklearn enforces this.
- **6.4** When agglomerative clustering is fitted, the full merge history (the linkage
  matrix) is available,
  not merely the final labels.
- **6.5** When a linkage matrix exists, a **dendrogram** structure can be
  derived — merge order, heights, and leaf ordering — as data, so `cajeta-
  chart` can render it (§8.5 of that spec) without this library drawing
  anything.
- **6.6** When the **cophenetic correlation** is computed, the result is how
  faithfully the dendrogram preserves the original pairwise distances — the
  standard quality check for hierarchical clustering.
- **6.7** When the tree at a height or a cluster count is cuted, the result is
  flat labels, so a single fit answers many `k`.

---

## 7. Feature: density-based clustering

- **7.1** When `DBSCAN` with `eps` and `minSamples` is fitted, clusters are
  formed from density-connected points.
- **7.2** When a point is in no dense region, it is labelled **noise** rather
  than forced into a cluster — the property that distinguishes DBSCAN from
  every algorithm in §4 and §6.
- **7.3** When DBSCAN is fitted, the cluster count is discovered, not specified
  — the other property that distinguishes it.
- **7.4** When a metric is used, any §2 metric applies.
- **7.5** When the result is read, core, border, and noise points are
  distinguishable.

---

## 8. Feature: cluster evaluation

`Metrics` has no unsupervised metric of any kind.

- **8.1** When the **silhouette score** is computed, the result is the mean
  over samples, and per-sample values are also available — per-sample
  silhouettes are what silhouette *plots* need.
- **8.2** When a clustering has one cluster, or as many clusters as points,
  silhouette is undefined and says so rather than returning a misleading
  number.
- **8.3** When candidate cluster counts is compared, silhouette, Davies-
  Bouldin, and Calinski-Harabasz are all available, so the choice of `k` rests
  on more than one statistic.
- **8.4** When ground-truth labels exist, adjusted Rand index and
  normalized mutual information are available for benchmarking.
- **8.5** When `k` is sweeped, inertia is retrievable across fits so an elbow
  curve is constructible.

---

## 9. Feature: manifold learning and divergence

- **9.1** When **KL divergence** between two discrete distributions is
  computed, the result is `Σ p(x)·log(p(x)/q(x))`, matching scipy's `rel_entr`
  summed.
- **9.2** When `q(x) = 0` where `p(x) > 0`, the divergence is infinite and
  reported as such, not silently `NaN`.
- **9.3** When the API is read, it is documented as **asymmetric** — `KL(p‖q) ≠
  KL(q‖p)` — which callers routinely get backwards.
- **9.4** When **t-SNE** with `nComponents` and `perplexity` is fitted, high-
  dimensional data is embedded in 2 or 3 dimensions by minimizing the KL
  divergence between the high- and low-dimensional neighbor distributions.
- **9.5** When t-SNE with a seed is fitted, the embedding is reproducible —
  t-SNE is randomly initialized and unseeded runs differ every time.
- **9.6** When the t-SNE docs is read, they state that **distances between
  clusters in the embedding are not meaningful** and that perplexity materially
  changes the picture. This is the most over-interpreted plot in data science
  and the library should not encourage it.
- **9.7** When t-SNE is used, it is a `Transformer` with `fitTransform` only —
  t-SNE cannot embed unseen points, and offering a `transform` that appears to
  would be a lie.

*(9.8–9.16 added 2026-08-05 — the embedding family beyond t-SNE.)*

- **9.8** When **MDS** is fitted, a low-dimensional embedding minimizes
  **stress** against the input dissimilarities via SMACOF, seeded and with
  `nInit` restarts keeping the best — sklearn's `MDS`. The final stress is
  exposed, since it is the fit-quality number.
- **9.9** When **non-metric MDS** is requested, only the rank order of the
  dissimilarities is preserved (isotonic regression inside SMACOF), and the
  variant is named rather than a flag defaulting silently.
- **9.10** When MDS is given a precomputed dissimilarity matrix (a §2 `pdist`
  result or any caller-supplied one), it embeds it directly — the door in when
  all that exists is dissimilarities, and the reason MDS composes with
  `cajeta.math.distance` rather than owning metrics.
- **9.11** When **Isomap** with `nNeighbors` is fitted, geodesic distances are
  approximated by shortest paths over the k-NN graph and embedded by classical
  MDS (the Gram double-centering + eigendecomposition), matching sklearn's
  `Isomap`. A disconnected neighbor graph is reported loudly, naming
  `nNeighbors` as the dial — the classic silent-wrongness of manifold methods.
- **9.12** When **LLE** (locally linear embedding, the standard variant) with
  `nNeighbors` is fitted, reconstruction weights are solved per point with the
  documented regularization and the embedding comes from the bottom non-trivial
  eigenvectors of `(I−W)ᵀ(I−W)`, matching sklearn's `LocallyLinearEmbedding`.
- **9.13** When **Laplacian eigenmaps** (spectral embedding) is fitted, the
  embedding is the bottom non-trivial eigenvectors of the normalized graph
  Laplacian over a k-NN or RBF affinity, matching sklearn's
  `SpectralEmbedding`; the dropped trivial eigenvector is documented, not a
  surprise.
- **9.14** When any §9.8–§9.13 method is used, which of them can embed UNSEEN
  points is stated per method and enforced by shape: Isomap and LLE provide
  `transform` (sklearn's out-of-sample extensions); MDS and spectral embedding
  are `fitTransform`-only, §9.7's honesty rule.
- **9.15** When the eigen-solvers behind §9.11–§9.13 run, they are exact and
  dense first — §11.3's doctrine — with the O(n²)–O(n³) limits documented per
  method.
- **9.16** When seeded (§9.8's SMACOF; any randomized initialization), runs
  reproduce exactly; the deterministic spectral methods are documented as
  deterministic up to eigenvector sign, and comparisons account for it.

---

## 10. Feature: linear decomposition beyond PCA

*(Added 2026-08-05 with §9.8–§9.16 — sklearn's `decomposition` module.)*

- **10.1** When **FastICA** with `nComponents` is fitted, statistically
  independent components are recovered by the fixed-point iteration with the
  `logcosh` contrast default (`exp` available), whitening via PCA, seeded —
  matching sklearn's `FastICA`. Non-convergence within `maxIter` is reported
  loudly, never returned as if converged.
- **10.2** When ICA results are compared (to the oracle or across runs), the
  documentation states the inherent **sign and permutation indeterminacy** —
  components have no canonical order or sign, and the test suite compares up
  to it rather than pinning raw coordinates.
- **10.3** When **NMF** with `nComponents` is fitted on non-negative data,
  `X ≈ W·H` with `W, H ≥ 0` minimizes the Frobenius objective with sklearn's
  coordinate-descent solver and `nndsvd` initialization (seeded where
  randomized); the reconstruction error is exposed. Negative input is rejected
  naming the offending entry — never clipped silently.
- **10.4** When NMF components are read, they are the parts-based, additive
  factors that are the reason to choose NMF over PCA — non-negativity is
  asserted on both factors, not assumed.
- **10.5** When **Factor Analysis** with `nComponents` is fitted, loadings and
  PER-FEATURE noise variances are estimated by EM, matching sklearn's
  `FactorAnalysis`; the log-likelihood is exposed and non-decreasing across EM
  iterations (§5.6's assertion, same reason).
- **10.6** When choosing between PCA and Factor Analysis, the documentation
  states the actual distinction — FA models heteroscedastic per-feature noise
  where PCA assumes none — rather than presenting them as interchangeable.
- **10.7** When any §10 estimator is used, it is a `Transformer` composing in
  `Pipeline`, with `transform` mapping new data through the fitted components
  (all three support it; `inverseTransform` where sklearn has it).

---

## 11. Open questions (resolve at plan time)

- **11.1** *(resolved — superseded by roadmap §5.1.)* The §2 kernels live in
  **`cajeta.math.distance`**, not in `dev.cajeta.ml`. This spec's earlier
  recommendation of an ml-owned module was **wrong**: `dev.cajeta.recsys` is a
  separate library, so an ml-owned kernel would force it to depend on the entire
  ML library to compute a cosine. `stdlib-completion` §2 owns the work.
- **11.2** *(resolved.)* KL divergence lives in **`cajeta.math.stats`**, owned
  by `stdlib-completion` §5. It is a general information-theoretic quantity and
  t-SNE is merely its first consumer. This spec consumes it.
- **11.3** *(resolved 2026-08-01 — exact first.)* t-SNE ships exact, with the
  O(n²) limit documented. Correctness is checkable against an oracle;
  Barnes-Hut is a large second effort and is deferred until the limit bites.
- **11.4** *(resolved 2026-08-01 — include.)* `KMedoids` ships despite
  scikit-learn-extra's marginal status. Its metric flexibility (§4.3) has no
  substitute in the K-means family, and medoids are the robust-to-outliers
  answer K-means cannot give.
- **11.5** *(resolved 2026-08-01 — the split holds.)* §6.5 emits dendrogram
  *data* — merge order, heights, leaf ordering — and `dev.cajeta.chart` renders
  it (`cajeta-chart` §8.5). This library does not draw.
- **11.6** The §9.11–§9.13 spectral methods and §10 decompositions need a
  SYMMETRIC EIGENSOLVER (`eigh`-class: smallest/largest eigenpairs of a dense
  symmetric matrix). `LinAlg` today has `svd`/`cholesky`/`slogdet`. Resolve at
  plan time whether `svd` suffices (it does for the PSD Gram/Laplacian cases,
  at some cost and with the smallest-eigenpair order inverted) or `LinAlg`
  gains `eigh`. **If `eigh` is needed it is STDLIB work and must ride the next
  toolchain cut** (the P1 doctrine — a stdlib gap blocks consumers for a whole
  release; v0.17.0 has just shipped, so surface this before any other work in
  this plan).
- **11.7** Isomap's geodesic step is all-pairs shortest paths over the k-NN
  graph — which `dev.cajeta.graph` (shipped 0.1.0) already implements. Resolve:
  depend on `dev.cajeta.graph`, or implement the small Dijkstra internally.
  Recommendation: INTERNAL — one algorithm's private step does not justify a
  cross-library dependency edge, and the roadmap keeps `dev.cajeta.ml`'s
  dependency set stdlib-only.
- **11.8** NMF objective scope: Frobenius only in v1 (sklearn's default
  solver); the beta-divergence family (KL/Itakura-Saito, multiplicative
  updates) is deferred until a consumer names it.

---

## 12. Acceptance criteria (spec-level)

- **12.1** Every estimator conforms to `Estimator`/`Predictor`/`Transformer`
  and composes in `Pipeline`, except where §9.7 documents why it cannot.
- **12.2** Numerics pin against scikit-learn 1.9.0, and K-medoids against the
  pinned scikit-learn-extra; divergences recorded.
- **12.3** GMM's log-likelihood is non-decreasing across EM iterations,
  asserted by test (§5.6).
- **12.4** Ward linkage with a non-Euclidean metric is rejected (§6.3).
- **12.5** DBSCAN labels noise points as noise and does not require a cluster
  count (§7.2, §7.3).
- **12.6** Every algorithm is tested on the shapes that separate them:
  spherical blobs (K-means wins), elongated and nested non-convex shapes
  (DBSCAN wins), and overlapping Gaussians (GMM wins). A clustering suite that
  only tests blobs proves nothing.
- **12.7** Seeded fits are reproducible across runs and thread counts.
- **12.8** Distance kernels are implemented once and shared (§11.1), verified
  by there being no second implementation in the tree.
- **12.9** §9.8–§9.16 and §10 numerics pin against scikit-learn 1.9.0 UP TO
  EACH METHOD'S DOCUMENTED INDETERMINACY: ICA up to sign and permutation, NMF
  up to factor permutation at fixed seed/init, spectral embeddings up to
  eigenvector sign, MDS by stress value and pairwise-distance structure
  (Procrustes-aligned), never raw coordinates where no canonical form exists.
- **12.10** Isomap reports a disconnected neighbor graph loudly (§9.11), and
  every `fitTransform`-only method rejects `transform` per §9.14.
