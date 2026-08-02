# ml-trees-ensembles — CART, pruning, bagging, forests, and boosting

## 1. Definition

### 1.1 Purpose

`dev.cajeta.ml` has no tree model of any kind. `dev.cajeta.xgboost` has a
gradient-boosted histogram tree builder, but that is a different algorithm with
a different purpose — it is not a CART, and it exposes no classifier. This spec
adds the tree family: a standalone decision tree for classification and
regression, the pruning and regularization that make it usable, and the
ensembles built on it (bagging, random forests, AdaBoost, gradient boosting).

### 1.2 Scope basis

The standard tree and ensemble surface — CART with entropy/gini splitting,
cost-complexity pruning, bagging, random forests, and the boosting family —
scoped against scikit-learn's `tree` and `ensemble` modules.

### 1.3 Scope

`DecisionTree{Classifier,Regressor}`, `Bagging{Classifier,Regressor}`,
`RandomForest{Classifier,Regressor}`, `AdaBoost{Classifier,Regressor}`,
`GradientBoosting{Classifier,Regressor}`, and closing the `XGBClassifier` gap in
`dev.cajeta.xgboost`.

### 1.4 Non-goals

- **1.4.1** Stacking and voting ensembles. No consumer yet.
- **1.4.2** Isolation Forest (anomaly detection) — a different problem.
- **1.4.3** Extra-Trees. The `random` splitter seam is designed in (§3.6) but
  the estimator is deferred.
- **1.4.4** GPU tree building. `cajeta-xgboost` owns that phase.
- **1.4.5** Multi-way categorical splits — see §11.1; resolved as out of scope.

### 1.5 Systems

`cajeta.math.Tensor`, `cajeta.math.random.Generator`,
`dev.cajeta.ml` (`Estimator`/`Predictor`/`Transformer`, `Metrics`, `Split`),
`dev.cajeta.xgboost` (`tree/SplitFinder`, `Histogram` — candidate reuse),
`dev.cajeta.unit`.

### 1.6 The parity contract

scikit-learn 1.9.0 is the oracle, never a port — the discipline the library
already follows. The reference checkout is `/home/julian/code/cpp/sklearn-ref`.

### 1.7 Binary splits, settled

sklearn's tree node carries exactly `left_child` and `right_child`
(`sklearn/tree/_tree.pxd`). **Every split is binary.** Expositions that split a
three-category feature three ways are presenting the concept, not the
implementation. Categorical features reach a tree through the encoders in
`ml-classification-gaps-spec.md` §8.

---

## 2. Feature: the decision tree

- **2.1** When `DecisionTreeClassifier` on `(x, y)` is fitted, a binary tree is
  grown by recursively choosing the split that maximizes impurity decrease, and
  prediction routes a row to a leaf and returns that leaf's majority class.
- **2.2** When `DecisionTreeRegressor` is fitted, leaves predict the mean of
  their training targets.
- **2.3** When `predictProba` on a classifier is called, the result is each
  leaf's class distribution as fractions.
- **2.4** When a node is pure, it becomes a leaf and is not split further.
- **2.5** When `depth()` and `leafCount()` on a fitted tree is read, the result
  is the realized structure, so the pruning effects in §4 are observable rather
  than asserted.
- **2.6** When a tree is fitted with a fixed seed, it is identical across runs,
  including tie-breaking between equally good splits.
- **2.7** When two splits have exactly equal impurity decrease, the tie is
  broken by a documented, deterministic rule.
- **2.8** When a fitted tree is exported, the result is a readable rule dump —
  the decision rules along each root-to-leaf path. Interpretability is the
  tree's headline advantage and must be exercisable.

---

## 3. Feature: split criteria

Entropy and information gain are conventionally derived in **log base 2**, and
sklearn's `entropy` criterion agrees.

- **3.1** When the `entropy` criterion is selected, node impurity is `H(Y) = −Σ
  pᵢ log₂ pᵢ` and the split score is the weighted impurity decrease `H(parent)
  − [P(S₁)H(S₁) + P(S₂)H(S₂)]` — information gain.
- **3.2** When a class has zero probability in a node, its `0·log 0` term
  contributes zero rather than `NaN`.
- **3.3** When `gini` (the default) is selected, impurity is `1 − Σ pᵢ²`.
- **3.4** When a regressor is fitted, `squared_error`, `absolute_error`, and
  `poisson` are all selectable.
- **3.5** When the hand-computed boolean example is reproduced, the library
  agrees: `H(Y) = 0.954`, split on `X₁` gives `IG = 0.5485`, split on `X₂` gives
  `IG = 0.0485`, and `X₁` is chosen.
- **3.6** When a `random` splitter is selected instead of `best`, the split
  threshold is drawn at random per feature — the Extra-Trees seam, present as a
  strategy even though the estimator is deferred.

---

## 4. Feature: pruning and regularization

Overfitting is the tree's central limitation and pruning is the remedy: start at
the leaves, walk upward, replace an insignificant subtree with a leaf carrying
the majority class.

- **4.1** When `maxDepth` is set, no path exceeds it.
- **4.2** When `minSamplesSplit` or `minSamplesLeaf` is set, no split is made
  that would violate either.
- **4.3** When `maxLeafNodes` is set, the tree is grown best-first and stops at
  that many leaves.
- **4.4** When `minImpurityDecrease` is set, a split whose weighted impurity
  decrease falls below it is not taken.
- **4.5** When `ccpAlpha` is set, cost-complexity pruning runs after growth:
  subtrees whose complexity cost exceeds their impurity benefit are collapsed
  to leaves, bottom-up — sklearn's `ccp_alpha`.
- **4.6** When a subtree is collapsed, the resulting leaf predicts the majority
  class over all samples the subtree held, and the tree's depth shrinks
  accordingly.
- **4.7** When the cost-complexity pruning path is requested, the result is the
  effective alphas and their impurities, so alpha can be chosen by cross-
  validation rather than guessed.
- **4.8** When class weights is passed, they scale each sample's contribution
  to impurity. Imbalanced problems need this — it is required, not optional.

---

## 5. Feature: feature importance

Built-in feature selection is a headline advantage of trees: fitting one
produces a ranking of features by relevance for free.

- **5.1** When `featureImportances()` on a fitted tree is read, the result is
  the normalized total impurity decrease attributed to each feature, summing to
  1.
- **5.2** When a feature is never split on, its importance is exactly zero.
- **5.3** When importances from an ensemble are read, they are averaged across
  the fitted base estimators.

---

## 6. Feature: bootstrap and bagging

Bagging = **B**ootstrap + **Agg**regation.

- **6.1** When a bootstrap sample of size `n` is drawn, it is drawn with
  replacement under a seed and is reproducible.
- **6.2** When a `BaggingClassifier` over a base estimator is fitted, each base
  model trains on its own bootstrap sample and prediction aggregates by
  majority vote; the regressor averages.
- **6.3** When `maxSamples` or `maxFeatures` is set, each base model sees that
  fraction of rows or columns.
- **6.4** When `bootstrapFeatures` is enabled, features are sampled with
  replacement too.
- **6.5** When `oobScore` is enabled, each sample is scored only by the base
  models that did not train on it, and the out-of-bag estimate is reported.
- **6.6** When no sample is out-of-bag for any estimator, the OOB score is not
  silently `NaN` — it warns and says why.
- **6.7** When any estimator conforming to the `Predictor` protocol is baged,
  it works — bagging is not tree-specific.

---

## 7. Feature: random forests

A forest is bagging over trees, plus **per-node feature subsampling** — the
latter is what separates it from plain bagging.

- **7.1** When a `RandomForestClassifier` is fitted, each tree trains on a
  bootstrap sample and, **at every node**, considers only a random subset of
  features for the split.
- **7.2** When `maxFeatures` unset is left, the default is `sqrt(p)` for
  classification and `p` for regression, matching sklearn.
- **7.3** When a forest predicts, classification aggregates by majority vote
  and regression by mean.
- **7.4** When a forest is fitted with a seed, the whole forest — bootstrap
  draws and per-node feature draws — is reproducible.
- **7.5** When `nEstimators` is set, that many trees are grown, and every per-
  tree regularization parameter from §4 is honored.
- **7.6** When the OOB score on a forest is requested, §6.5 applies unchanged.

---

## 8. Feature: boosting

AdaBoost and gradient boosting sit alongside bagging: sequential error
correction rather than parallel variance reduction.

- **8.1** When an `AdaBoostClassifier` is fitted, base models are fitted
  sequentially on reweighted samples, misclassified samples gain weight, and
  prediction is a weighted vote by estimator quality.
- **8.2** When `learningRate` is set, each estimator's contribution is shrunk
  by it.
- **8.3** When a `GradientBoostingRegressor` is fitted, each stage fits a tree
  to the negative gradient of the loss at the current prediction, and stages
  accumulate scaled by the learning rate.
- **8.4** When `subsample` below 1 is set, each stage fits on a random
  subsample (stochastic gradient boosting) under the seed.
- **8.5** When a gradient-boosting classifier is fitted, binary and multiclass
  targets are both supported, with the multiclass strategy documented.
- **8.6** When `GradientBoosting*` against `dev.cajeta.xgboost` is compared,
  the docs state plainly that these are different algorithms with different
  defaults and are not expected to agree numerically.

---

## 9. Feature: closing the XGBoost classifier gap

`dev.cajeta.xgboost.api` ships `XGBRegressor` only. `XGBClassifier` is the more
commonly used of the two and is absent.

- **9.1** When an `XGBClassifier` is fitted, binary and multiclass
  classification work through the existing booster, with the logistic and
  softmax objectives.
- **9.2** When `predictProba` is called, the result is calibrated probabilities
  from the raw margin scores.
- **9.3** When `XGBClassifier` is used, it conforms to `dev.cajeta.ml`'s
  `Predictor` protocol via the existing adapter, so it composes in `Pipeline`
  and cross-validation like any other model.
- **9.4** When it is fitted, the bit-exact parity discipline in `cajeta-
  xgboost-spec.md` applies unchanged — this adds an objective and an API
  surface, not a new numeric contract.

---

## 10. Feature: validation against reference notebooks

The reference notebooks under `research/ml/` ship committed cell outputs under
`random_state = 1`, which makes them usable as parity fixtures rather than mere
examples.

- **10.1** When the HR Employee Attrition pipeline in cajeta is run, the
  decision-tree metrics match the notebook's committed values within the
  suite's tolerance.
- **10.2** When a reference fixture disagrees with cajeta, the investigation
  starts by confirming the library version — fixtures were produced under an
  older scikit-learn than the pinned 1.9.0, and version drift is the first
  suspect, not the last.
- **10.3** When a reference notebook leaves a step unseeded, it is not usable as a
  fixture and is recorded as such rather than pinned to a number that will not
  reproduce.
- **10.4** When a fixture is added, the source notebook itself stays out of git
  — only regenerated numbers are committed.

---

## 11. Open questions (resolve at plan time)

- **11.1** *(resolved, recorded for traceability)* Multi-way categorical
  splits: **no**. sklearn is binary-only (§1.7); categorical features go
  through encoders; the three-way-split presentation is expository.
- **11.2** *(resolved 2026-08-01 — rewrite.)* The CART splitter is written
  fresh rather than reusing `cajeta-xgboost`'s `SplitFinder`/`Histogram`, which
  are built for gradient statistics on binned features where CART needs exact
  splits on raw values with class counts. Revisit sharing only if a histogram
  mode is added later.
- **11.3** *(resolved — see roadmap §3.)* The tree estimators live in
  **`dev.cajeta.ml`** alongside the classical surface. They are part of the same
  toolkit and depend on its protocol; only `XGBClassifier` (§9) sits in
  `dev.cajeta.xgboost`, where its booster already is.
- **11.4** *(resolved 2026-08-01 — last, and cuttable.)* `GradientBoosting*` is
  sequenced **last** in this spec's plan and is explicitly **cuttable** if its
  parity cost outruns its value. It is a genuinely different algorithm from
  XGBoost's, but it is also the most expensive item here and `dev.cajeta.xgboost`
  already covers the common need.
- **11.5** *(resolved 2026-08-01 — generic.)* Bagging is generic over
  `Predictor` (§6.7) from v1. It is barely more work than the tree-specific
  form, it is what sklearn does, and it makes bagging usable with any estimator
  in the ecosystem.

---

## 12. Acceptance criteria (spec-level)

- **12.1** Every estimator conforms to `Estimator`/`Predictor` and composes in
  `Pipeline` and cross-validation without special-casing.
- **12.2** Every numeric claim is pinned against a scikit-learn 1.9.0 fixture;
  deviations are recorded in `DifferencesFromSklearn`.
- **12.3** The hand-computed entropy example (§3.5) is a test.
- **12.4** Seeded fits are bit-reproducible across runs and thread counts.
- **12.5** Pruning is observable: a test asserts that raising `ccpAlpha`
  monotonically reduces leaf count.
- **12.6** No estimator silently ignores a parameter it does not support.
