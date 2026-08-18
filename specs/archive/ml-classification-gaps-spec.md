# ml-classification-gaps — Gaussian discriminants, k-NN parity, and cost-aware logistic regression

## 1. Definition

### 1.1 Purpose

`dev.cajeta.ml` covers the classification surface partially. This spec closes
the gaps: the Gaussian generative classifiers (LDA/QDA) are absent entirely,
k-NN is fixed to Euclidean with no distance weighting on the classifier,
logistic regression has no cost-sensitivity and no L1 penalty, and model
selection has no stratification, no repeated holdout, and no hyperparameter
search.

### 1.2 Scope basis

The standard supervised-classification surface: logistic regression, k-nearest
neighbours, and the Gaussian discriminants (LDA/QDA), together with the model
selection, encoding, and metric machinery they require. scikit-learn defines the
expected API and coverage.

### 1.3 Scope

Twelve gaps, grouped into seven features (§2–§8). Everything lands in
`dev.cajeta.ml` except where §8 says otherwise.

### 1.4 Non-goals

- **1.4.1** SVM and neural networks. The neural surface is `cajeta-ml-v3`'s;
  SVM is deferred until something needs it.
- **1.4.2** Decision trees and random forests — `ml-trees-ensembles-spec.md`.
- **1.4.3** Approximate nearest neighbors (kd-tree, ball-tree, HNSW). Brute
  force stays the only k-NN backend; the metric seam is designed so an index
  can be added later without an API change.
- **1.4.4** GPU kernels.

### 1.5 Systems

`cajeta.math.Tensor`, `cajeta.math.linalg.LinAlg` (svd, eigh, cholesky, solve,
slogdet, pinv), `cajeta.math.stats.Stats`, `cajeta.math.random.Generator`,
`dev.cajeta.unit`.

### 1.6 The parity contract

Unchanged from the library's existing discipline: scikit-learn is the oracle,
never a port. Every estimator added here pins against scikit-learn 1.9.0
fixtures — coefficients, probabilities, and decision values — at the tolerance
the existing suite uses. Where this library deliberately differs, it is recorded
in `DifferencesFromSklearn` and the difference is loud, not silent.

---

## 2. Feature: Linear Discriminant Analysis

The model-based Gaussian approach: estimate a class prior `π_k` and a Gaussian
class-conditional `N(μ_k, C)` per class with **one shared** `C`, then predict the
class maximizing the log posterior. Shared covariance makes the boundary
linear.

- **2.1** When `LinearDiscriminantAnalysis.fit(x, y)` is called, class priors
  are estimated as class frequencies, per-class means `μ_k` are estimated, and
  one pooled within-class covariance is estimated.
- **2.2** When explicit priors to the constructor is passed, those are used
  instead of the empirical frequencies, and priors that do not sum to 1 or
  contain a negative entry are rejected loudly.
- **2.3** When `predict(x)` is called, each row is assigned the class
  maximizing `log(π_k) − ½(x−μ_k)ᵀC⁻¹(x−μ_k)` — the log-posterior rule.
- **2.4** When `predictProba(x)` is called, the result is normalized posteriors
  `P(Y=k|X)` per row, computed in log space with a max-shift so no row
  underflows.
- **2.5** When `decisionFunction(x)` is called, the result is the linear scores
  `xᵀΣ⁻¹μ_k − ½μ_kᵀΣ⁻¹μ_k + log π_k`, one column per class (a single column for
  the binary case, sklearn's convention).
- **2.6** When the pooled covariance is singular or ill-conditioned, the fit
  does not silently produce garbage: it either routes through the SVD solver or
  raises with the condition number named.
- **2.7** When the `svd` solver (the default) is chosen, the fit never forms
  the covariance matrix explicitly and works when `p > n`.
- **2.8** When the `lsqr` or `eigen` solver with a shrinkage value in `[0,1]`
  is chosen, the pooled covariance is shrunk toward a scaled identity by that
  intensity.
- **2.9** When shrinkage to `auto` is set, the Ledoit-Wolf analytic intensity
  is computed from the data (`ledoit-wolf-2004-wellconditioned-
  covariance.pdf`).
- **2.10** When shrinkage with the `svd` solver is requested, the call is
  rejected with a message naming the supported solvers, rather than ignoring
  the parameter.
- **2.11** When `transform(x)` after fitting is called, the result is the data
  projected onto at most `min(K−1, p)` discriminant directions — the
  minimize-within-class, maximize-between-class view, and the basis for LDA as a
  dimensionality reduction step in a `Pipeline`.
- **2.12** When `explainedVarianceRatio()` is read, the result is the share of
  between-class variance on each discriminant direction.

---

## 3. Feature: Quadratic Discriminant Analysis

Same generative model, one covariance **per class**. The `C_k`-dependent term no
longer cancels between classes, so the boundary is quadratic.

- **3.1** When `QuadraticDiscriminantAnalysis.fit(x, y)` is called, a separate
  `μ_k` and `C_k` are estimated per class.
- **3.2** When `predict(x)` is called, each row takes the class maximizing `log
  π_k − ½ log|C_k| − ½(x−μ_k)ᵀC_k⁻¹(x−μ_k)`, including the log-determinant term
  that LDA drops.
- **3.3** When a class has fewer observations than features, `C_k` is singular
  and the fit warns loudly naming the class and the remedy (regularization, or
  LDA).
- **3.4** When `regParam` in `[0,1]` is set, each `C_k` is regularized toward a
  scaled identity before inversion, per Friedman 1989.
- **3.5** When QDA is fitted, per-class covariances are factored once at fit
  time (SVD of the centered class block) and prediction reuses the factors — no
  per-prediction inversion.
- **3.6** When `storeCovariance` is requested, the fitted per-class covariance
  matrices are retained and readable.
- **3.7** When LDA and QDA on the same data is compared, both expose the same
  `Predictor` surface so they are interchangeable in a `Pipeline` and in cross-
  validation.

---

## 4. Feature: Shared Gaussian-classifier machinery

- **4.1** When either discriminant estimates a covariance, it uses one shared,
  tested covariance estimator rather than a private copy.
- **4.2** When a shrunk covariance is needed outside LDA/QDA, the
  Ledoit-Wolf estimator is callable on its own.
- **4.3** When a log-posterior is normalized, one shared log-sum-exp helper
  does it, so LDA, QDA, and multinomial logistic agree numerically.

---

## 5. Feature: k-NN parity — metrics and distance weighting

k-NN's distance step admits any metric — Euclidean, Manhattan, and the rest —
and weighted nearest neighbors is a standard variant. Today
`KNeighborsClassifier` is Euclidean-only with uniform votes, while
`KNeighborsRegressor` already has distance weighting — an asymmetry to remove.

- **5.1** When either k-NN estimator with a metric is constructed, `euclidean`,
  `manhattan`, `chebyshev`, and `minkowski(p)` are all available, with
  `euclidean` the default.
- **5.2** When `minkowski` with `p ≤ 0` is passed, the constructor rejects it.
- **5.3** When `KNeighborsClassifier` with distance weights is constructed,
  votes are weighted by `1/d` and `predictProba` returns weighted vote
  fractions.
- **5.4** When a test point exactly coincides with one or more training points
  under distance weighting, those exact matches take all the weight — the same
  rule `KNeighborsRegressor` already implements.
- **5.5** When votes tie, the lowest label wins, unchanged from today and
  matching sklearn's argmax.
- **5.6** When a metric is added, it is one distance function written against a
  single internal seam — the neighbor search, voting, and tie- breaking are
  metric-agnostic.
- **5.7** When features before k-NN with `StandardScaler` inside a `Pipeline`
  are scaled, the pipeline composes with any metric. Scale sensitivity needs no
  new API but does need a worked example in the docs.

---

## 6. Feature: Cost-aware and sparse logistic regression

Unbalanced data and asymmetric costs call for weighting the log-likelihood per
class (`log L = Σ P(Y=0|X) + w·Σ P(Y=1|X)`), for setting the decision cut
somewhere other than 0.5, and — on the loan-default example in §12.3 — for an L1
penalty, which is the best-performing model there.

- **6.1** When per-class weights to `LogisticRegression` is passed, each
  observation's contribution to the log-likelihood and to the IRLS weights is
  scaled by its class weight.
- **6.2** When `balanced` instead of explicit weights is passed, weights are
  set to `n / (K · n_k)`, sklearn's convention.
- **6.3** When per-observation sample weights is passed, they compose
  multiplicatively with class weights.
- **6.4** When `predict(x, threshold)` is called, the binary decision uses that
  probability cut instead of 0.5; the no-argument `predict` keeps today's
  behaviour so existing callers are unaffected.
- **6.5** When a threshold outside `(0,1)` is supplied, or one is used on a multiclass
  fit, the call is rejected with a message saying why.
- **6.6** When the L1 penalty is selected, the fit minimizes the L1-penalized
  negative log-likelihood by coordinate descent (`friedman-2010-glmnet-
  coordinate-descent.pdf`), the intercept stays unpenalized, and exact zeros
  appear in the coefficients.
- **6.7** When the elastic-net penalty with a mixing ratio is selected, `ratio
  = 1` reproduces the L1 fit and `ratio = 0` reproduces the L2 fit.
- **6.8** When an L1 or elastic-net fit is used, `summary()` does not report
  standard errors, t-statistics, or p-values — they are not valid for a
  penalized fit — and says so rather than printing numbers that look
  meaningful.
- **6.9** When an L1 fit does not converge within `maxIter`, the warning is as
  loud as the existing IRLS non-convergence warning and `converged()` reports
  it.

---

## 7. Feature: Model selection — stratification, repeated holdout, and search

The standard way to pick `K`: set aside a validation fraction and, for every
candidate value, refit many times and compare average performance. On a
3.33%-positive dataset, unstratified splits are the wrong default.

- **7.1** When data is split or folded with stratification requested, each part
  preserves the class proportions of the full sample.
- **7.2** When a class has fewer members than the fold count, stratification
  fails loudly naming the class rather than producing an empty fold.
- **7.3** When a repeated random holdout with a split count and a test fraction
  is run, the result is that many independent seeded splits.
- **7.4** When a repeated holdout is scored, the result is the per- split
  scores and their mean and standard deviation, not just the mean.
- **7.5** When a grid search over a parameter grid with a cross-validation
  strategy and a metric is run, the result is the best parameter combination,
  its score, and the full per-combination score table.
- **7.6** When a randomized search with a budget is run, that many combinations
  are sampled from the grid under a seed (`bergstra-2012-random-search-
  hyperparameter.pdf`).
- **7.7** When a search runs twice with the same seed, the results are
  identical — search is reproducible.
- **7.8** When a metric name is passed, accuracy, F1, and ROC- AUC are all
  selectable, so unbalanced problems are not scored on accuracy by default.
- **7.9** When k is swept for a k-NN classifier, the result is a K-versus-validation-
  error table that can be read off directly, reproducible from the library
  alone.
- **7.10** When a single parameter combination throws during search, the search
  records the failure and continues rather than aborting the sweep.

---

## 8. Feature: Categorical encoding

Real feature sets are continuous, categorical, or a mix. The library's
estimators take `Tensor<float64>`, so categorical inputs have no route in
today.

- **8.1** When a one-hot encoder on a categorical column is fitted, the result
  is an indicator column per observed category, in a stable, documented order.
- **8.2** When drop-first is requested, one category is dropped, so the
  encoding is usable in a model with an intercept.
- **8.3** When data containing a category unseen at fit time is transformed,
  the configured policy applies — raise, or encode as all-zero — and the
  default is to raise.
- **8.4** When an ordinal encoder is fitted, each category maps to an integer
  code under an explicit supplied ordering or the sorted order by
  default.
- **8.5** When an encoder in a `Pipeline` is put, it satisfies the existing
  `Transformer` protocol with no special-casing.
- **8.6** *(Open — see §11.1)* When categorical data lives in a
  `nucleo.frame.Table` rather than a numeric tensor, there is one documented
  route from table to encoded tensor.

---

## 9. Feature: Kernel (local) regression

Kernel regression is weighted nearest neighbors taken to its limit: fit a
regression line from the points in the vicinity of the target point. Nothing in
the library does this.

- **9.1** When a Nadaraya-Watson kernel regressor and predict is fitted, each
  prediction is the kernel-weighted mean of training targets.
- **9.2** When a kernel is chosen, gaussian, tricube, and epanechnikov are
  available.
- **9.3** When the bandwidth is set, it controls the neighborhood width, and a
  non-positive bandwidth is rejected.
- **9.4** When bandwidth by cross-validation is selected, it goes through the
  same §7 search machinery as any other hyperparameter.
- **9.5** When every training point has negligible weight for a query point,
  the prediction is not a silent `NaN` — the estimator raises or falls back
  under a documented, explicit rule.

---

## 10. Feature: reporting, curves, and feature selection

*Added 2026-07-31, post-approval — reporting and curve surface missed by the
first pass. Everything above is unchanged.*

- **10.1** When a precision-recall curve is computed, the result is precision,
  recall, and the thresholds that produced them, so the trade-off can be
  inspected across the decision threshold rather than at 0.5 only. This pairs
  with the threshold knob in §6.4 — the curve is how a threshold gets chosen.
- **10.2** When average precision from that curve is computed, the result is
  the summary statistic, so imbalanced problems have a single number that is
  not accuracy.
- **10.3** When a classification report is requested, the result is per-class
  precision, recall, F1, and support, plus macro and weighted averages — the
  shape sklearn reports in, and therefore the shape parity fixtures must be
  compared in.
- **10.4** When forward feature selection over an estimator is run, features
  are added greedily by cross-validated score until a target count or a no-
  improvement stop, and the selected subset is readable.
- **10.5** When feature selection is used, it satisfies the `Transformer`
  protocol so it composes in a `Pipeline`.
- **10.6** When a bootstrap resample is drawn, it is drawn with replacement
  under a seed and is reproducible. *(Shared with `ml-trees-ensembles-spec`
  §6.1 — implement once, in whichever lands first.)*

---

## 11. Open questions (resolve at plan time)

- **11.1** *(resolved 2026-08-01 — ml, against tensors.)* Encoders are
  implemented in `dev.cajeta.ml` over tensors (§8.1–8.5); the `Table` route
  (§8.6) is deferred until nucleo's own encoding story is settled. The
  `Transformer` protocol and `Pipeline` live in ml, and splitting the encoders
  from them would be worse than the imperfect fit.
- **11.2** *(resolved 2026-08-01 — coordinate descent.)* The L1 solver is
  glmnet-style coordinate descent, not OWL-QN. It agrees with sklearn's
  `liblinear`/`saga` path at these problem sizes and matches the paper already
  in the corpus.
- **11.3** *(resolved 2026-08-01 — yes.)* LDA implements `Transformer`, so its
  `transform` is usable as a supervised dimensionality reducer beside `PCA` and
  composes in a `Pipeline`.
- **11.4** *(resolved 2026-08-01 — keep it.)* Kernel regression stays in this
  spec despite being its only regression-side item. It is small and it is a k-NN
  variant, so it shares §5's neighbour machinery.
- **11.5** *(resolved 2026-08-01 — documentation only.)* Much of the literature
  draws the confusion matrix transposed relative to `Metrics.confusionMatrix`
  (predicted on rows, true on columns). The code follows sklearn and **does not
  change**; the orientation is stated explicitly in the docs so a reader
  comparing against a textbook is not misled.

---

## 12. Acceptance criteria (spec-level)

- **12.1** Every estimator added conforms to the existing `Estimator` /
  `Predictor` / `Transformer` protocols and composes in `Pipeline` and in
  cross-validation without special-casing.
- **12.2** Every numeric claim is pinned against a scikit-learn 1.9.0 fixture
  at the tolerance the existing suite uses; deviations are recorded in
  `DifferencesFromSklearn`.
- **12.3** The loan-default worked example is reproducible from
  the library alone, producing LDA, QDA, logistic, and L1-logistic
  misclassification rates and a K-versus-error sweep for k-NN.
- **12.4** No estimator silently ignores a parameter it does not support.
- **12.5** Existing constructors and call sites keep working; every addition is
  either a new overload or a new type.
