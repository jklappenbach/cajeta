# stdlib-completion — the stdlib additions the data-science roadmap requires

## 1. Definition

### 1.1 Purpose

Eleven specs in `datascience-platform-roadmap-spec.md` depend on stdlib
capabilities that do not exist. None of those specs owns them, so without this
one they are orphaned — every consuming plan would either re-implement them or
stall.

This spec collects them. Each passes the roadmap's §1.3.1 test: **domain-neutral
— useful to someone who will never fit a model.**

### 1.2 Why one spec rather than folding each into its first consumer

The distance kernels have **three** consumers across **three separate
libraries** (`dev.cajeta.ml`, `dev.cajeta.recsys`, and clustering). Folding them
into whichever plan runs first would either bury a shared facility inside one
library or duplicate it. The rest are small and share the same character, so
they travel together.

### 1.3 Scope

| Addition | Package | Blocks |
|---|---|---|
| Distance & similarity kernels | `cajeta.math` | `ml-classification-gaps` §5, `ml-unsupervised` §2, `ml-recsys` §4 |
| Discrete distributions | `cajeta.math.stats` | statistical coverage |
| Hypothesis testing | `cajeta.math.stats` | statistical coverage |
| KL divergence | `cajeta.math.stats` | `ml-unsupervised` §9 (t-SNE) |
| **Numerical optimization** — L-BFGS, Nelder-Mead | `cajeta.math.optim` | `ml-timeseries` §8 (ARMA/ARIMA MLE) |
| OKLab colour space | `cajeta.math.Color` | `cajeta-chart` §10.2 palette validation |
| Unicode normalization | `cajeta.lang.String` | `cajeta-docs` §7.1 |

### 1.4 Non-goals

- **1.4.1** `nucleo.frame`'s `EXCHANGE` node, pluggable `Exec`, and nested
  column types. Also stdlib, but intimately tied to `nucleo-distributed-frame`;
  they belong to that spec's plan.
- **1.4.2** Continuous distributions beyond what §3 names. `normalCdf` and
  `tCdf` already exist.
- **1.4.3** A general statistics library. Scope is exactly the consumers in
  §1.3.
- **1.4.4** Approximate nearest-neighbour indexes. `nucleo-frame-spec` §9.4.3
  already owns HNSW/IVF.

### 1.5 Systems

`cajeta.math.Tensor`, `cajeta.math.stats.Stats` (existing `gammaLn`, `betainc`,
`erf`, `normalCdf`, `tCdf`), `cajeta.math.Color`, `cajeta.lang.String`,
`dev.cajeta.unit`.

---

## 2. Feature: distance and similarity kernels

Three libraries need these. They are pure functions over tensors with no
estimator semantics, which is why they belong here rather than in
`dev.cajeta.ml` — an ml-owned kernel would force `dev.cajeta.recsys` to depend on
the entire ML library to compute a cosine (roadmap §5.1).

- **2.1** When a distance between two vectors is computed, **euclidean**,
  **manhattan**, **chebyshev**, **minkowski(p)**, and **cosine** are available
  behind one interface.
- **2.2** When `minkowski` a non-positive `p` is passed, it is rejected.
- **2.3** When pairwise distances over `n` points is computed, the result is
  the `(n, n)` matrix, with symmetry and a zero diagonal guaranteed by
  construction rather than by rounding.
- **2.4** When distances between two different sets are computed, the result is
  the cross-distance matrix — scipy's `cdist` to §2.3's `pdist`.
- **2.5** When a vector has zero norm and I ask for cosine, the result is
  defined rather than a division by zero.
- **2.6** When **Pearson correlation** as a similarity is computed, it is
  available beside cosine — `ml-recsys` §4.1 needs both.
- **2.7** When a metric is added, every consumer gains it; no algorithm
  hardcodes a distance.
- **2.8** When a large pairwise matrix is computed, euclidean distances use the
  expanded `‖a‖² + ‖b‖² − 2a·b` form so the work becomes one matmul, and the
  documented caveat is that it loses precision for near-zero distances relative
  to the direct form.

---

## 3. Feature: discrete distributions

The binomial family is the gap. The machinery already exists —
`gammaLn` gives a stable log binomial coefficient and `betainc` is the standard
route to the binomial CDF — so this is an **API gap, not a numerics gap**.

- **3.1** When the binomial PMF is computed, the result is
  `C(n,x)·pˣ·(1−p)^(n−x)`, evaluated in log space so large `n` does not
  overflow.
- **3.2** When the binomial CDF is computed, it is available via the
  regularized incomplete beta.
- **3.3** When `n = 1`, the Bernoulli case is exact and named as such.
- **3.4** When the Poisson distribution is used, PMF and CDF are available.
- **3.5** When any of these is sampled, sampling is seeded through
  `cajeta.math.random.Generator` and reproducible.
- **3.6** When parameters are invalid (`p ∉ [0,1]`, negative `n`), it is
  rejected rather than returning `NaN`.

---

## 4. Feature: hypothesis testing

`Stats` has `normalCdf` and `tCdf` but no test API. Nothing in the ecosystem
provides one, and hypothesis testing is table stakes for a statistics package.

- **4.1** When a **one-sample t-test** is run, the result is the statistic,
  degrees of freedom, and p-value.
- **4.2** When a **two-sample t-test** is run, equal-variance and Welch's
  unequal-variance forms are both available, and **which one is the default is
  stated** — they disagree, and libraries differ on the default.
- **4.3** When a **paired t-test** is run, it is distinct from the two-sample
  form.
- **4.4** When a **one- or two-tailed** alternative is chosen, it is explicit,
  never inferred.
- **4.5** When a **chi-square** test is run, goodness-of-fit and independence
  (contingency table) forms are both available.
- **4.6** When a **one-way ANOVA** is run, the result is the F statistic and
  p-value.
- **4.7** When a test's assumptions are violated (too few samples, zero
  variance, expected cell counts below the usual threshold), it warns — a
  p-value from a violated assumption is worse than no p-value.
- **4.8** When a result is read, it carries the statistic, p-value, degrees of
  freedom, and the alternative tested, so it can be reported without
  recomputation.

---

## 5. Feature: information theory

- **5.1** When **KL divergence** between two discrete distributions is
  computed, the result is `Σ p(x)·log(p(x)/q(x))`, matching scipy's `rel_entr`
  summed.
- **5.2** When `q(x) = 0` where `p(x) > 0`, the divergence is **infinite and
  reported as such**, not silently `NaN`.
- **5.3** When `p(x) = 0`, that term contributes zero, per the `0·log 0 = 0`
  convention.
- **5.4** When the API is read, it is documented as **asymmetric** — `KL(p‖q) ≠
  KL(q‖p)` — which callers routinely get backwards.
- **5.5** When **entropy** or **cross-entropy** is computed, they are available
  beside it, with the log base explicit. `ml-trees-ensembles` §3.1 needs base
  2; ML losses conventionally use base e.

---

## 6. Feature: OKLab

`cajeta-chart` §10.2 validates palettes for colour-vision-deficiency separation
and contrast. Those checks are perceptual and cannot be computed in sRGB.

- **6.1** When a `Color` to **OKLab** is converted, the result is its
  perceptual lightness and the two chroma axes.
- **6.2** When a colour is converted back, the round trip is within a stated tolerance.
- **6.3** When a perceptual difference (ΔE) between two colours is computed, it
  is available in OKLab.
- **6.4** When **OKLCh** (polar form) is converted, it is available — hue and
  chroma are what a palette generator manipulates.
- **6.5** When a conversion produces a colour outside the sRGB gamut, that is
  detectable rather than silently clipped.

---

## 7. Feature: Unicode normalization

`cajeta.lang.String` has `toLowerCase`, `toUpperCase`, and `trim` but no
normalization. This is **string correctness, not NLP**: composed and decomposed
forms of identical text must compare equal, or text read from two sources fails
to match itself with no visible cause.

- **7.1** When a string is normalized, **NFC, NFD, NFKC, and NFKD** are all
  available and named.
- **7.2** When strings that differ only in normalization form are compared,
  normalizing both makes them equal.
- **7.3** When text is case-folded for comparison, **full Unicode case folding** is
  available and distinguished from `toLowerCase` — they differ for several
  scripts, and only folding is correct for caseless matching.
- **7.4** When a string is already in the requested form, normalization is
  cheap — the common case must not copy.
- **7.5** When default-ignorable characters (soft hyphens, zero-width joiners)
  is striped, that is available separately from normalization, since the two
  are distinct operations often wanted together.

---

## 8. Open questions (resolve at plan time)

- **8.1** *(resolved 2026-08-01.)* The §2 kernels live in a
  `cajeta.math.distance` subpackage rather than at `cajeta.math`'s top level —
  the set will grow and that top level is already broad.
- **8.2** *(resolved 2026-08-01.)* §2.8's expanded-form optimization stays in
  the spec. The precision caveat is **observable behaviour**, not an internal
  detail, so it is a requirement rather than a plan-time choice.
- **8.3** *(resolved 2026-08-01 — build it.)* §4 hypothesis testing has no
  consumer among the eleven specs, but it is small and
  `LogisticRegression.summary()` already reports p-values, so the primitives are
  half-present anyway.
- **8.4** *(resolved 2026-08-01 — the full UCD.)* §7 normalization ships the
  **complete Unicode character database** rather than binding a platform ICU.
  It is the largest item here by bytes shipped and that is accepted: a bundled
  UCD makes normalization identical on every platform, which §7.2's
  compare-equal property depends on, and an ICU binding would make results vary
  by the host's ICU version. It also leaves collation and segmentation reachable
  later without a second data decision.
  - **8.4.1** *(follow-on)* The UCD is large enough that its packaging is a real
    question: generated tables compiled in, or a `cajeta.resource` asset loaded
    at first use? Decide when §7's plan opens; the second keeps the binary
    smaller but makes normalization depend on resource resolution.
- **8.5** *(resolved 2026-08-01.)* §5.5's entropy lives here, not in
  `dev.cajeta.ml` beside the tree criteria — it is a general quantity and trees
  are one consumer.
- **8.6** *(new, from `ml-timeseries` §10.2 — 2026-08-01.)* **`cajeta.math`
  gains a numerical optimizer**: L-BFGS and Nelder-Mead. ARMA/ARIMA maximum
  likelihood needs one and nothing in the ecosystem provides it; it passes the
  §1.3.1 domain-neutral test, and putting it in `dev.cajeta.timeseries` would
  repeat the mistake the distance kernels nearly made. Open sub-question: does
  it need analytic gradients, or is numerical differentiation sufficient for the
  likelihoods in scope? Recommendation: numerical first, with the seam for
  analytic.

---

## 9. Acceptance criteria (spec-level)

- **9.1** Distance kernels are implemented **once** and shared, verified by
  there being no second implementation anywhere in the ecosystem (`ml-
  unsupervised` §11.8).
- **9.2** Pairwise distance matrices are exactly symmetric with an exact zero
  diagonal (§2.3).
- **9.3** The binomial PMF matches hand-computed values:
  `P(X=2 | n=6, p=0.5) = 0.234` and `P(X=7 | n=10, p=0.8) = 0.201` — checkable
  without an oracle.
- **9.4** Binomial PMF is stable for large `n` where a naive factorial form
  overflows (§3.1).
- **9.5** Hypothesis tests pin against scipy 1.18.0, **with the variance
  assumption exercised in both forms** (§4.2) — matching only the default would
  hide the difference.
- **9.6** KL divergence returns infinity, not `NaN`, for the `q=0, p>0` case
  (§5.2).
- **9.7** OKLab round-trips within tolerance, and the palette validator in
  `cajeta-chart` §10.2 runs against it (§6.1–§6.3).
- **9.8** Strings differing only in normalization form compare equal after
  normalization (§7.2) — the property `cajeta-docs` §13.13 depends on.
- **9.9** Every addition is additive: no existing `Stats`, `Color`, or `String`
  call site changes behaviour.
