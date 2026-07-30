# cajeta-ml — Specification

> Status: draft for review (2026-07-29). A new **external library** (`cajeta-ml`
> repo, package `dev.cajeta.ml`) — the scikit-learn/statsmodels role for the
> cajeta ecosystem, per the layering doctrine of `python-stack-analysis.md`
> §4.7: estimators live outside the stdlib; the numerics they stand on live in
> `cajeta.math` (dense solvers arriving via `specs/linalg-solvers-spec.md`).
> Reference sources pinned: scikit-learn **1.9.0** at `code/ml/sklearn-ref`,
> scipy **v1.18.0** at `code/ml/scipy-ref`. Recognizable-not-faithful (analysis
> §2.5): sklearn's API shape, corrected where its conventions encode mistakes.
> Plan-time decisions marked `[M*]`, collected in §10.

## 1. Definition

### 1.1 Purpose
Classical (non-deep) ML for cajeta: estimator objects with train/predict
lifecycles over tensors and `Table<T>` dataframes. v1 is the linear-model
family — the commissioning use case — plus the preprocessing, model-selection,
and metrics surface every estimator needs. Beyond its own models, cajeta-ml
**owns the ecosystem estimator protocol** (§2): the contract `cajeta-xgboost`
and every future model library conforms to.

### 1.2 Scope (v1)
- The **estimator protocol**: `Estimator`/`Predictor`/`Transformer` surfaces.
- **Linear models**: `LinearRegression` (QR lstsq), `Ridge` (Cholesky),
  `LogisticRegression` (newton-cholesky/IRLS; binary + one-vs-rest).
- **Inference summary** for linear models: stderr, t-statistics, p-values,
  R²/adjusted-R², condition warning (the statsmodels half, scoped small).
- **Preprocessing**: `StandardScaler`, `MinMaxScaler`.
- **Model selection**: `trainTestSplit`, `KFold`, `crossValScore`.
- **Metrics**: mse/rmse/mae/r², accuracy/precision/recall/f1, confusion
  matrix, log-loss, ROC-AUC.
- **Stdlib prerequisite (cross-repo)**: `cajeta.math.special` — `erf`/`erfc`,
  `expit`, `betainc` (+ normal/t CDFs in `cajeta.math.stats`). Small pure
  primitives; per §4.7 they belong in the stdlib, and logistic + p-values
  need them.
- **Ecosystem deliverables, first-class**: repo scaffold, manifest-first
  `cajeta.json`, CI/CD (build/test on main, tag → GitHub Release + Olla
  publish, self-arming on `OLLA_*` secrets), self-checking tour, `docs/`.

### 1.3 Non-goals (v1)
- Trees/ensembles/SVM/kNN/NB/clustering/PCA-as-estimator — later versions;
  the protocol is designed so they slot in.
- Lasso/ElasticNet (coordinate descent) and lbfgs-style optimizers — deferred;
  first likely home is a `cajeta-optimize` domain lib when a second consumer
  appears.
- `Pipeline`/`ColumnTransformer` composition — deferred (v1 proves the
  protocol; composition rides on top of it).
- Multinomial (softmax) logistic — OvR covers multiclass in v1.
- Sparse design matrices — arrives with `nucleo.sparse`.
- GPU training paths.
- sklearn's `get_params`/`set_params`/clone reflection machinery — cajeta
  constructors + records make it unnecessary.

### 1.4 Relationship to existing constructs
- **`cajeta.math.linalg`** (via `specs/linalg-solvers-spec.md`): `lstsq` (QR,
  rectangular, multi-RHS), `cholesky`+`choSolve`, `svd`, `cond` — the fit
  engines. Units of this spec that need them are blocked until that plan's
  Units 1–4 land.
- **`nucleo.frame`**: `Table<T>` is an ingestion source (the xgboost pattern) —
  convenience overloads accept tables; the core operates on `Tensor`.
- **`cajeta-xgboost`**: conforms to §2's protocol via a thin adapter in its
  own repo (recorded follow-up, not a v1 unit here).
- **`cajeta-unit`**: the test framework, dev-dependency, resolved by the
  standard run-tests.sh ladder.

## 2. Feature: the estimator protocol

The ecosystem-wide contract. Kept minimal:

- `Estimator` — `fit(X, y)`; fitted state lives on the instance;
  hyperparameters are constructor arguments (no `set_params` mutation).
- `Predictor extends Estimator` — `predict(X)`, `score(X, y)` (R² for
  regressors, accuracy for classifiers — the sklearn convention);
  classifiers add `predictProba(X)`.
- `Transformer extends Estimator` — `transform(X)`, `fitTransform(X)`.
- Fit results that carry more than coefficients return **records** (e.g.
  `SummaryResult`), the ecosystem return convention.

Use cases:
- 2.1 `crossValScore` accepts any `Predictor` — including, later, an adapted
  `cajeta-xgboost` model — without knowing its type.
- 2.2 A scaler and a model share the fit lifecycle, enabling future
  `Pipeline` composition without protocol change.
- 2.3 `cajeta-xgboost` publishes an adapter and immediately gains
  `trainTestSplit`/`KFold`/metrics interop.

## 3. Feature: data ingestion

`X` is a `Tensor<float64>` `(n_samples, n_features)`, `y` a `(n,)` tensor
`[M2]`. Convenience overloads take `Table<T>` (+ feature/target column
selection) and lower to tensors via the frame's zero-copy column seam.

Use cases:
- 3.1 Fit from a Parquet-loaded `Table<T>` in two lines.
- 3.2 Fit from raw tensors with no dataframe in sight (the numpy workflow).

## 4. Feature: LinearRegression

Ordinary least squares via `LinAlg.lstsq` (QR path), the sklearn algorithm.
`fitIntercept` (default true) via column centering, like sklearn. Multi-output
`y (n,k)` via multi-RHS. Fitted surface: `coef`, `intercept`, plus `rank` and
singular-value diagnostics (sklearn keeps these; so do we).

Use cases:
- 4.1 The canonical fit/predict/score regression workflow.
- 4.2 Multi-output regression in one fit.
- 4.3 Rank-deficiency surfaced (diagnostics + documented minimum-norm
  behavior) instead of silently unstable coefficients.

## 5. Feature: Ridge

L2-regularized regression: solve `(XᵀX + λI)·w = Xᵀy` via
`cholesky`+`choSolve` (sklearn's dense `auto` choice); `svd` solver option
for near-singular cases. Intercept never regularized (centering).

Use cases:
- 5.1 Regularized fit where OLS is ill-conditioned.
- 5.2 λ-path: refit across a λ grid cheaply (Gram matrix formed once);
  selected by `crossValScore`.

## 6. Feature: LogisticRegression

Binary via **newton-cholesky (IRLS)**: Newton steps on the log-loss Hessian,
each step a `choSolve` — sklearn's `newton-cholesky` solver, and the right
default at cajeta-ml's scale (no lbfgs dependency). Optional L2 (`C`
parameter, sklearn convention `[M3]`). Multiclass via one-vs-rest. `expit`
from `cajeta.math.special`. Convergence: `maxIter`/`tol` with a loud
non-convergence warning (not sklearn's silent one).

Use cases:
- 6.1 Binary classification with `predictProba` for thresholding.
- 6.2 Multiclass via OvR, same API.
- 6.3 Regularized (`C`) vs unregularized (statsmodels-style) fits — both
  reachable, defaults documented honestly (sklearn regularizes by default
  and surprises statisticians; we document and keep `[M3]`).

## 7. Feature: inference summary

`summary()` on fitted linear models → a record: per-coefficient stderr,
t-statistic, p-value (exact t-distribution CDF via `betainc`), R²/adjusted R²,
F-statistic, and a condition-number warning above a documented threshold.
The statsmodels half, scoped to what regression consumers actually read.

Use cases:
- 7.1 "Is this coefficient significant?" without leaving cajeta.
- 7.2 Regression tables in reports (tour demonstrates rendering one).
- 7.3 Multicollinearity flagged via `cond` before coefficients are trusted.

## 8. Feature: preprocessing, model selection, metrics

- `StandardScaler`/`MinMaxScaler` (Transformers; fit stores stats, transform
  applies; inverse-transform included).
- `trainTestSplit(X, y, testFraction, seed)` — seeded via `cajeta.math.random`.
- `KFold(k, shuffle, seed)` + `crossValScore(est, X, y, folds)`.
- Metrics as pure static functions over tensors: mse/rmse/mae/r²; accuracy,
  precision/recall/f1 (binary + macro), confusion matrix, log-loss, ROC-AUC
  (sort-based).

Use cases:
- 8.1 The standard scale → split → fit → score loop, end to end.
- 8.2 k-fold CV for λ/C selection.
- 8.3 Honest classifier evaluation beyond accuracy (imbalanced classes).

## 9. Ecosystem deliverables

The library-archetype set, first-class from day one (the xgboost lesson —
there they were bolted on afterward):
- Repo `cajeta-ml`, manifest-first `cajeta.json` (`dev.cajeta.ml`, version
  single-sourced), `run-tests.sh` with the unit-resolution ladder.
- CI: build+test on main pushes; tag `v*` → build, tag==manifest assertion,
  GitHub Release, Olla publish self-arming on `OLLA_*` secrets (cajeta-logging
  bridge fallback).
- Self-checking `cajeta tour` walking §2–§8 end to end.
- `docs/` markdown: README, Guide, Tour walkthrough, and an honest
  "Differences from scikit-learn" page.
- Tests pinned against **sklearn 1.9.0 / scipy 1.18.0 / statsmodels-computed
  fixtures** (committed constants or npy, the xgboost fixture convention).

## 10. Plan-time decisions

- **[M1] v1 estimator cut** — as §1.2 (three linear models) vs adding
  Lasso/ElasticNet. Lean: three; coordinate descent is its own unit of risk.
- **[M2] Element type** — `float64` only vs generic `Floating`. Lean: f64
  only in v1 (sklearn's choice); the protocol doesn't preclude widening.
- **[M3] Logistic regularization default** — sklearn's `C=1.0`-on-by-default
  vs unregularized-by-default. Lean: follow sklearn (`C=1.0`) for muscle
  memory, document loudly; `summary()` warns when regularized (p-values on a
  penalized fit are not classical inference).
- **[M4] Where `special` lands** — one stdlib commit inside this plan
  (cross-repo unit, the nucleo-frame U17 precedent) vs a separate micro-spec.
  Lean: cross-repo unit here; it's ~4 functions with scipy-pinned tests.
- **[M5] Protocol packaging** — protocol types in `dev.cajeta.ml` proper vs a
  separate tiny `dev.cajeta.ml.api` package so conformers avoid the full dep.
  Lean: single package in v1; split only if an external conformer needs it
  (the xgboost adapter will tell us).
