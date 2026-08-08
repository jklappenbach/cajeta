# ml-repackaging — subpackage taxonomy for dev.cajeta.ml

## 1. Definition

### 1.1 Purpose
`dev.cajeta.ml`'s root package holds 76 classes — linear models, trees,
ensembles, clustering, manifolds, preprocessing, model selection, metrics,
and the ecosystem estimator protocol, all in one flat namespace.
Discoverability now depends on naming conventions rather than structure
(user-raised, 2026-08-07). This spec groups the root into subpackages under
`dev.cajeta.ml` without splitting the library or changing any behaviour.

### 1.2 Non-goals
- **1.2.1** No library split: one repo, one archive, one version.
- **1.2.2** No API changes beyond package placement: every class keeps its
  name, visibility, and surface.
- **1.2.3** No re-export/alias layer — cajeta has none; this is a clean
  breaking move taken once.
- **1.2.4** The existing `nn`/`optim`/`grad`/`train`/`zoo`/`io`/`data`
  subpackages are untouched.

## 2. The taxonomy

- **2.1** The ROOT keeps only the ecosystem contract and cross-cutting glue:
  `Estimator`, `Predictor`, `Transformer`, `ProbClassifier`,
  `EstimatorFactory`, `MlException`, `Metrics`, `Pipeline`, `Design`,
  `Frames`, `Ml`. Rationale: these are what sibling libraries import
  (xgboost, timeseries, recsys pin exactly this set plus model selection) —
  keeping them fixed minimizes ecosystem churn.
- **2.2** `.linear` — LinearRegression, Ridge, Lasso, ElasticNet,
  LogisticRegression, SummaryResult, DummyRegressor.
- **2.3** `.discriminant` — LinearDiscriminantAnalysis,
  QuadraticDiscriminantAnalysis, Gaussians.
- **2.4** `.tree` — DecisionTreeClassifier/Regressor, Trees, the Impurity
  family (Impurity, Gini/Entropy/SquaredError/AbsoluteError/Poisson),
  Criteria, PruningPath, TreeClassifierFactory, TreeRegressorFactory.
- **2.5** `.ensemble` — Bagging, BagMember, BaggingClassifier/Regressor,
  RandomForestClassifier/Regressor, AdaBoostClassifier,
  GradientBoostingRegressor.
- **2.6** `.cluster` — KMeans, KMedoids, DBSCAN, AgglomerativeClustering,
  GaussianMixture.
- **2.7** `.decompose` — PCA, NMF, FactorAnalysis, FastICA.
- **2.8** `.manifold` — TSNE, Isomap, LLE, MDS, SpectralEmbedding.
- **2.9** `.neighbors` — KNeighborsClassifier/Regressor, KernelRegressor.
- **2.10** `.preprocess` — StandardScaler, MinMaxScaler, OneHotEncoder,
  OrdinalEncoder, IdentityTransformer.
- **2.11** `.select` — KFold, StratifiedKFold, Split, RepeatedHoldout,
  HoldoutResult, GridSearch, RandomizedSearch, SearchResult,
  ForwardSelector, Scorers.
- **2.12** `.inspect` — ClassificationReport, PrCurve.

## 3. Constraints verified at spec time

- **3.1** Package-private clusters stay intact: the Impurity family +
  `Trees` are referenced only within `.tree`; `Bagging`/`BagMember` only
  within `.ensemble`. No visibility widening is required.
- **3.2** Ensembles consume trees only through the PUBLIC
  `DecisionTree*`/factory surface — a clean cross-package import.
- **3.3** Consumers: `dev.cajeta.timeseries` and `dev.cajeta.recsys` import
  ONLY root classes (`Metrics`) — no code change, pin bump only.
  `cajeta-xgboost` additionally imports `KFold` and `Split` → two import
  updates + pin bump.

## 4. Versioning and sequencing

- **4.1** cajeta-ml releases as **0.10.0**; 0.9.0 stays on Olla, so
  already-published consumers keep resolving.
- **4.2** This taxonomy ANSWERS `cajeta-ml-dist` U1.2.1: distributed
  training does NOT become `dev.cajeta.ml.dist` (split package across
  archives); the dist library takes its own root package (settled there).

## 5. Acceptance

- **5.1** Suite + tour green, bit-identical numerics (packages don't touch
  math).
- **5.2** Root contains exactly the §2.1 set (plus the pre-existing
  subpackages).
- **5.3** xgboost/timeseries/recsys build against 0.10.0 with only the §3.3
  changes.
- **5.4** 0.10.0 on Olla, resolve 200.
