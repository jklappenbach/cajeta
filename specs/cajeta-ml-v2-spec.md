# cajeta-ml v2 — the deferred scope (regularized paths, softmax, Pipeline, sparse, the classical tail)

## 1. Definition

cajeta-ml 0.3.0: everything the v1 spec consciously cut and recorded as
"slots into the protocol unchanged" — regularized linear paths
(Lasso/ElasticNet via coordinate descent), multinomial softmax logistic
regression, `Pipeline` composition, sparse design matrices (landing
`cajeta.nucleo.sparse` in the stdlib as their substrate), and the classical
tail (KMeans, k-nearest-neighbors, PCA). Same doctrine as v1: recognizable
sklearn surface, numerics pinned against sklearn 1.9.0
(`code/ml/sklearn-ref`), determinism by construction (explicit seeds, no
global RNG), loud `MlException` failures, everything a `Predictor`/
`Transformer` conformer so `Split.crossValScore` and `Pipeline` drive it
generically.

## 2. Feature: Lasso / ElasticNet (coordinate descent)

sklearn's `Lasso(alpha)` / `ElasticNet(alpha, l1Ratio)`: cyclic coordinate
descent on the (optionally centered) design, sklearn's objective
`1/(2n)·‖y−Xw‖² + α·l1r·‖w‖₁ + α·(1−l1r)/2·‖w‖²`, soft-threshold updates,
sklearn's stopping rule (max coef update vs `tol·max(var)` — the cd
contract), `maxIter` with the v1 LOUD non-convergence convention.

Use cases:
- 2.1 Canonical fit/predict/score; coefficients pinned against sklearn
  1.9.0 fixtures to documented tolerance.
- 2.2 Sparsity is real: large-enough alpha zeroes exactly the coefficients
  sklearn zeroes; `sparsityCount()` surfaces it.
- 2.3 ElasticNet spans the bridge: `l1Ratio = 1` reproduces Lasso,
  `l1Ratio → 0` approaches Ridge behavior (pinned at both ends).
- 2.4 Non-convergence is loud (stdout + `converged()`), last iterate
  returned.

## 3. Feature: multinomial softmax LogisticRegression

`LogisticRegression(c, tol, maxIter, multinomial=true)` (constructor knob —
no silent strategy switching): true softmax likelihood via damped full
Newton on the stacked `(p+1)·(K−1)` system (reference-class coefficients,
class K pinned at zero) with `LinAlg.choSolve` — the newton-cholesky house
style — L2 penalty `1/(2C)` on weights (never intercepts).

Use cases:
- 3.1 3-class fit: probabilities row-sum to 1; class predictions and
  probabilities match sklearn's multinomial mode to documented tolerance.
- 3.2 Binary multinomial degenerates to the v1 binary fit (same decision
  function).
- 3.3 `summary()` inherits the v1 Wald machinery with `penalized = true`
  labeling.
- 3.4 OvR remains the default (sklearn-compatible muscle memory);
  multinomial is the explicit opt-in.

## 4. Feature: Pipeline

`Pipeline` — a `Predictor` built from fitted stages: zero or more
`Transformer`s and a final `Estimator`/`Predictor`. `fit` chains
`fitTransform` left to right then fits the final stage; `predict`/`score`
chain `transform`. Because `Pipeline` IS a `Predictor`, it drops into
`crossValScore` (whole-pipeline refit per fold — the leakage-free shape)
and even nests.

Use cases:
- 4.1 `scaler → LinearRegression` pipeline: predictions identical to the
  hand-chained equivalent.
- 4.2 `crossValScore(pipeline, …)` refits the scaler INSIDE each fold —
  score differs from the leaky pre-scaled baseline on shifted data.
- 4.3 Construction: `Pipeline.of(...)` overloads (0–3 transformers +
  final); loud `MlException` on predict-before-fit, empty pipeline.
- 4.4 Nesting: a Pipeline as another Pipeline's final stage works.

## 5. Feature: sparse design matrices (`cajeta.nucleo.sparse`)

The retained nucleo-sparse type lands in the stdlib with exactly the
surface its first consumer needs (doctrine §4.7: type + primitives in
stdlib, algorithms external):

- `cajeta.nucleo.sparse.CsrMatrix` — float64 CSR: `rowPtr/colIdx/values`,
  `(n, p)` shape, builders (`fromCoo`, `fromDense`), `toDense`,
  `rowCount/colCount/nnz`, SpMV (`matVec`, `matVecT`), per-column access
  for cd (column norms, axpy into residual via a CSC mirror built once).
- cajeta-ml: `fitSparse(CsrMatrix x, Tensor y)` on Lasso/ElasticNet — the
  cd solver over sparse columns; dense and sparse fits agree bit-for-bit
  on the same data.

Use cases:
- 5.1 CSR round-trips (fromDense → toDense identity; fromCoo ordering).
- 5.2 SpMV/SpMVT match dense matmul on random patterns.
- 5.3 `Lasso.fitSparse` == `Lasso.fit` coefficients on identical data;
  memory-proportional-to-nnz is the point (documented, not asserted).
- 5.4 Toolchain lifecycle: sparse ships in a cajeta stdlib release
  (v0.13.0); the ml pin bumps.

## 6. Feature: the classical tail — KMeans, kNN, PCA

- **KMeans(k, seed, maxIter, tol)** — k-means++ initialization with the
  seeded splitmix-style RNG (deterministic), Lloyd iterations, `inertia()`,
  `centers()`, `predict` = nearest center, `Transformer` face optional-out
  (v2: predictor only). Pinned: on a well-separated fixture the label
  partition and inertia match sklearn (up to label permutation —
  the test canonicalizes).
- **KNeighborsRegressor / KNeighborsClassifier (k, weights)** — brute-force
  euclidean, `uniform`/`distance` weights, deterministic tie-break (lowest
  index — documented where sklearn's is unspecified). Pinned against
  sklearn on fixtures without ties.
- **PCA(nComponents)** — center, `LinAlg.svd`, components with sklearn's
  `svd_flip` sign convention, `explainedVariance`/`Ratio`, `transform`/
  `inverseTransform`; a `Transformer` (fits Pipelines). Pinned against
  sklearn fixtures.

Use cases:
- 6.1 Each estimator's canonical workflow + sklearn-pinned numerics.
- 6.2 `PCA → LinearRegression` Pipeline (principal-component regression)
  end to end.
- 6.3 Determinism: same seed ⇒ identical results; different seeds may
  differ (KMeans).

## 7. Deliverables & lifecycle

cajeta-ml 0.3.0 on Olla (tour + docs extended per feature; the fixture
generator scripts recorded under `tools/fixtures/` with the pinned sklearn
version); stdlib `cajeta.nucleo.sparse` in cajeta v0.13.0 with
compiler-repo tests; INDEX lifecycle per workflow.

## 8. Non-goals (v3+)

GPU/float32 paths, sample weights, sparse input for anything beyond the cd
family, MiniBatchKMeans, approximate NN (trees/LSH), DBSCAN/agglomerative,
IncrementalPCA, calibration, ColumnTransformer (Pipeline over heterogeneous
frames rides a later Frames extension).
