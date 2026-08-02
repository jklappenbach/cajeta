# ml-recsys — user-item recommendation: collaborative, content-based, and matrix estimation

## 1. Definition

### 1.1 Purpose

Cajeta has no recommendation capability and no text-vectorization capability.
`cajeta.nucleo.sparse.CsrMatrix` is the only relevant foundation. This spec
covers the recommender surface: the user-item interaction matrix, rating
baselines, neighborhood and model-based collaborative filtering, clustering-based
recommenders, content-based filtering over item text, and the top-N ranking
metrics none of the existing metric code provides.

### 1.2 Scope basis

The standard recommender surface: the user-item interaction matrix, rating
baselines, neighbourhood and matrix-factorization collaborative filtering,
clustering-based recommenders, content-based filtering, and top-N ranking
metrics — plus matrix estimation over time series (§10). Scoped against Surprise
for the collaborative-filtering algorithms and scikit-learn for vectorization.

The theoretical backbone, worth adding to `research/ml/papers/`: Goldberg et al.
1992 (the original collaborative-filtering paper), Linden/Smith/York 2003 (Amazon
item-to-item CF), Koren/Bell/Volinsky 2009 (matrix factorization for recommender
systems), and Lee/Li/Shah/Song 2016 plus Borgs/Chayes/Lee/Shah 2017 on sparse
matrix estimation — the theory behind §5 and §10.

### 1.3 The parity oracles — four, not one

Unusually, this domain has no single oracle:

- **Surprise** (`CoClustering`, and the `SVD`/`KNNBasic` family) — the
  collaborative-filtering algorithms and their rating-prediction semantics.
- **scikit-learn** — `TfidfVectorizer`, `cosine_similarity`.
- **NLTK** — `word_tokenize`, `stopwords`.
- **mSSA** — multivariate singular spectrum analysis (§10), from Devavrat Shah's
  group; `mSSA(rank=…)` / `update_model(…)`. A research-grade package: treat its
  behaviour as a reference, not a standard.

Pin a version of each in the plan. Where Surprise's behaviour is
underspecified or its defaults are idiosyncratic, prefer the documented
algorithm and record the divergence loudly — Surprise is a much smaller and
less rigorously maintained project than sklearn, and should not be treated as
authoritative the way sklearn 1.9.0 is.

### 1.4 Scope

Interaction data; baselines; neighborhood CF; matrix-factorization CF including
singular value thresholding; co-clustering; content-based filtering; the minimal
text pipeline that content-based filtering needs; ranking and rating metrics.

### 1.5 Non-goals

- **1.5.1** A general NLP library. §8 provides only the tokenize → stopword →
  TF-IDF path content-based filtering needs. Stemming, lemmatization, POS
  tagging, embeddings, and language models are explicitly out — see §11.1.
- **1.5.2** Neural recommenders. They belong on `cajeta-ml-v3`'s engine once
  §2–§9 exist, not here.
- **1.5.3** *(no longer deferred — see §10.)* Matrix estimation over time
  series has a specific, implementable algorithm and is now in scope.
- **1.5.4** Online / incremental updating, and serving infrastructure.
- **1.5.5** Implicit-feedback-specific objectives (BPR, WARP). Ratings first.

### 1.6 Systems

`cajeta.nucleo.sparse.CsrMatrix`, `cajeta.math.Tensor`,
`cajeta.math.linalg.LinAlg` (`svd`, `lstsq`), `cajeta.math.random.Generator`,
`dev.cajeta.ml` (`Estimator`/`Predictor`, `Metrics`, `Split`, `KMeans`),
`dev.cajeta.unit`.

---

## 2. Feature: the user-item interaction matrix

The central data structure: rows are users, columns are items, cells are
ratings, and **most cells are missing**. Missing is not zero — conflating
them is the defining bug of this domain.

- **2.1** When an interaction matrix from (user, item, rating) triples is
  built, the result is a sparse structure over `CsrMatrix` in which absent
  pairs are *missing*, distinct from a rating of zero.
- **2.2** When for a missing cell's value is asked, the result is an explicit
  "no interaction" answer, never a silent `0.0`.
- **2.3** When the matrix's density is read, the result is the observed
  fraction — real matrices are overwhelmingly empty,
  and the number that governs which algorithms are viable.
- **2.4** When user or item identifiers are arbitrary strings or non-contiguous
  integers, they are mapped to contiguous indices and can be mapped back, so
  external IDs survive a round trip.
- **2.5** When interaction data for evaluation is split, the split is over
  *interactions*, and every test user retains training history — an ordinary
  row split produces cold-start users and silently measures the wrong thing.
- **2.6** When a test user or item is unseen in training, prediction returns a
  documented fallback (the §3 baseline), not a crash and not a fabricated
  score.

---

## 3. Feature: baselines

Averaging: assume every user is the same and predict the item's mean. Cheap,
and the bar every real model must clear.

- **3.1** When a global-mean baseline is fitted, every prediction is the mean
  of all observed ratings.
- **3.2** When an item-mean or user-mean baseline is fitted, predictions are
  that item's or user's observed mean, falling back to the global mean where a
  row or column is empty.
- **3.3** When a bias baseline is fitted, predictions are `μ + b_user + b_item`
  with the biases estimated by regularized least squares.
- **3.4** When any recommender is evaluated, a baseline is runnable on the same
  data with the same metrics, so improvement is measured rather than assumed.

---

## 4. Feature: neighborhood collaborative filtering

- **4.1** When similarity between two users or items is computed, cosine,
  Pearson correlation, and mean-squared-difference are available.
- **4.2** When two users share no co-rated items, their similarity is defined
  (zero) rather than `NaN` from an empty sum.
- **4.3** When user-based k-NN CF is fitted, a rating is predicted from the `k`
  most similar users who rated that item, weighted by similarity.
- **4.4** When item-based k-NN CF is fitted, the same applies transposed, and
  both share one implementation over an orientation parameter.
- **4.5** When a minimum number of co-rated items is required, pairs below it
  are excluded — similarity computed from one shared rating is noise and must
  not dominate.
- **4.6** When fewer than `k` neighbors qualify, prediction uses those
  available and reports the count, rather than silently thinning.
- **4.7** When mean-centered (Z-score) neighborhood CF is used, each user's
  rating scale is normalized before combination, so a harsh rater and a
  generous rater are comparable.

---

## 5. Feature: matrix factorization and singular value thresholding

Latent-factor models over the interaction matrix, and **singular value
thresholding** for matrix completion.

> **Two different algorithms share the name "SVD", and both are in scope.** The
> textbook form fills missing entries with zero and takes a **truncated SVD** of
> the dense result (sklearn's `TruncatedSVD`). *Surprise's* `SVD` is
> Koren-style **SGD over the observed entries only** and never materializes a
> dense matrix. They give different answers. The library must name them
> distinctly and document the difference, because conflating them is the single
> most likely source of confusion in this feature.

- **5.0** When an interaction matrix is zero-filled and a truncated SVD of rank
  `f` taken, the result is the textbook estimator, and the documentation
  states plainly that zero-filling treats "not rated" as "rated zero" — a
  strong and usually wrong assumption that §2.1 exists to prevent elsewhere.
- **5.1** When an SGD matrix factorization with `f` latent factors is fitted,
  user and item factor matrices are estimated over the *observed* entries only
  — the dense SVD in `LinAlg` cannot be applied directly to a matrix with
  missing cells, and the spec must not pretend otherwise.
- **5.2** When the factorization is fitted by stochastic gradient descent,
  regularization and learning rate are configurable and the fit is seeded and
  reproducible.
- **5.3** When bias terms are included, the model is `μ + b_u + b_i + qᵢᵀpᵤ`,
  matching Surprise's `SVD`.
- **5.4** When singular value thresholding is applied, the singular values of
  the (filled) matrix are soft-thresholded at a chosen level and the low-rank
  reconstruction is returned — the standard method for matrix completion.
- **5.5** When the threshold by validation is chosen, it goes through the same
  search machinery as any other hyperparameter.
- **5.6** When the learned factors is read, they are inspectable, so latent
  dimensions can be examined rather than taken on faith.
- **5.7** When a rating outside the rating scale is predicted, it is clipped to
  the scale's bounds, and the scale is a property of the dataset rather than a
  hardcoded 1–5.

---

## 6. Feature: co-clustering

Surprise's `CoClustering` is the reference for the clustering-based
recommender.

- **6.1** When a co-clustering recommender is fitted, users and items are
  simultaneously assigned to clusters and a rating is predicted from the user
  cluster, item cluster, and co-cluster means.
- **6.2** When the user and item cluster counts is set, both are honored, and a
  fit with a fixed seed is reproducible.
- **6.3** When a co-cluster is empty, prediction falls back through user
  cluster → item cluster → global mean along a documented chain.

---

## 7. Feature: content-based filtering

The second family: use item and user *features* rather than the interaction
matrix alone.

- **7.1** When item profiles from item text is built, each item is a numeric
  vector via the §8 pipeline.
- **7.2** When items similar to a given item are requested, cosine similarity
  over item profiles ranks them, and the top `n` are returned with their
  scores.
- **7.3** When a user profile from that user's rated items is built, it is the
  rating-weighted combination of those item profiles.
- **7.4** When for a user is recommended, items already interacted with are
  excluded by default, and including them is explicit.
- **7.5** When a new item has features but no interactions, content-based
  recommendation still works for it — the cold-start property that motivates
  this family over §4 and §5.

---

## 8. Feature: the minimal text pipeline

Cajeta has no text vectorization. Content-based filtering needs exactly three
steps, and this spec provides only those.

- **8.1** When a document is tokenized, the result is its terms under a
  documented, testable rule for case, punctuation, and whitespace.
- **8.2** When stopwords are removed, a supplied list is applied, and a default
  English list ships.
- **8.3** When a TF-IDF vectorizer over a corpus is fitted, the result is a
  sparse document-term matrix, with the term-frequency and inverse-document-
  frequency variants and the smoothing and normalization choices stated
  explicitly — sklearn's defaults are specific and not the textbook formula.
- **8.4** When an unseen document is transformed, terms absent from the fitted
  vocabulary are ignored and the vocabulary does not grow.
- **8.5** When cosine similarity between rows of a matrix is computed, the
  result is the pairwise similarity matrix. *(Shared surface: the k-NN metrics
  in `ml-classification-gaps-spec` §5 need the same distance kernels. Implement
  once.)*
- **8.6** When a document vector has zero norm, its cosine similarity is
  defined (zero) rather than a division by zero.

---

## 9. Feature: evaluation

`Metrics` has no ranking metric of any kind. Recommendation is evaluated on a
**ranked top-N list**, not on pointwise accuracy.

- **9.1** When rating prediction is evaluated, RMSE and MAE over held-out
  interactions are available, reusing `Metrics`.
- **9.2** When a top-`k` recommendation list is evaluated, precision@k,
  recall@k, and F1@k are available, at a caller-chosen `k`.
- **9.3** When relevance is defined, the rating threshold that separates
  relevant from irrelevant is an explicit parameter, since precision@k is
  meaningless without it.
- **9.4** When a user has fewer than `k` relevant items, the metric's
  denominator convention is documented — this is where independent precision@k
  implementations most often disagree.
- **9.5** When ranking quality with graded relevance is evaluated, NDCG@k is
  available.
- **9.6** When coverage is evaluated, the fraction of the catalog ever
  recommended is reportable — a recommender that only ever suggests the ten
  most popular items scores well on §9.2 and is useless.

---

## 10. Feature: matrix estimation over time series

*Added 2026-08-01, once the algorithm was concrete enough to spec.*

A time series is turned into a matrix and completed — so a recommender's
machinery becomes a forecaster. Its selling point: **it assumes no
stationarity**, unlike everything in `ml-timeseries-spec` §4.

- **10.1** When a trajectory matrix from a series with window `L` is built,
  successive length-`L` windows become columns (a Hankel matrix), turning
  `X(1)…X(T)` into the `(L, T−L+1)` matrix the §5 estimators already consume.
- **10.2** When `L` is not chosen, the default is `L ≈ √T`.
- **10.3** When the series has missing values, they are missing *cells* of that
  matrix and §5's estimators fill them — imputation and forecasting become the
  same operation.
- **10.4** When a forecast is made, the next `L` values come from extending the
  completed matrix, and the result maps back to series order.
- **10.5** When this path is used, the docs state that **no stationarity
  assumption is made** and cross-reference `ml-timeseries-spec` §4, so the
  choice between ARIMA and matrix estimation is informed rather than
  accidental.
- **10.6** When several related series exist, multivariate SSA stacks their
  trajectory matrices and estimates jointly — the `mSSA` reference's `rank`
  parameter is the shared latent dimension.
- **10.7** When `rank` is set, it bounds the retained singular values, and its
  effect on smoothing versus fidelity is documented.

---

## 11. Open questions (resolve at plan time)

- **11.1** *(resolved 2026-08-01.)* §8's text pipeline is **not** owned here —
  it moves to `cajeta-docs`, which owns the whole text/document surface. This
  spec becomes a consumer of its tokenization and TF-IDF.
- **11.2** *(resolved — superseded by roadmap §5.1.)* The kernels live in
  **`cajeta.math.distance`**, owned by `stdlib-completion` §2. This spec's
  earlier recommendation of a `dev.cajeta.ml` module was **wrong** — it would
  make this library depend on the entire ML library to compute a cosine.
- **11.3** *(resolved 2026-08-01 — pin it, with judgement.)* Surprise is the
  oracle for the algorithms it covers, pinned to a version. It is small and
  lightly maintained next to sklearn, so a divergence is a **finding to
  investigate**, not an automatic cajeta bug — the opposite of the sklearn
  discipline, and stated so it is not applied by reflex.
- **11.4** *(resolved — see roadmap §4.)* **`dev.cajeta.recsys`**, a separate
  library. The estimator protocol fits awkwardly — recommenders predict for
  (user, item) pairs and return ranked lists, not `predict(x)` over feature
  rows — so it is reused only where it genuinely applies.
- **11.5** *(resolved 2026-08-01 — SGD first.)* §5.1 ships SGD matrix
  factorization; ALS is a later unit. ALS parallelizes better and is the
  industry default at scale, which makes it the natural first addition once
  `cajeta-ml-dist` exists.

---

## 12. Acceptance criteria (spec-level)

- **12.1** Missing and zero are distinguishable everywhere in §2, enforced by
  test — a rating of 0 and no rating must never compare equal.
- **12.2** Every algorithm is evaluated against the §3 baselines on the same
  split; a model that fails to beat item-mean is reported as such.
- **12.3** Rating-prediction numerics pin against the pinned Surprise version;
  TF-IDF and cosine pin against scikit-learn 1.9.0; divergences are recorded.
- **12.4** precision@k / recall@k are verified against a hand-computed example
  small enough to check on paper, because §9.4's convention is where
  implementations silently disagree.
- **12.5** Seeded fits are reproducible across runs and thread counts.
- **12.6** No recommender silently recommends items the user has already
  interacted with unless explicitly asked.
