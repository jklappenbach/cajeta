# Trees — Gradient-Boosted Trees — Specification

> Status: draft for review (2026-06-23). This is the Layer-2 third ML lineage,
> gradient-boosting in the XGBoost lineage, at `dev.cajeta.nucleo.trees`. Companion analysis:
> `python-stack-analysis.md` §3.6 (the gradient-boosting lineage), §4.2 (three lineages, one
> substrate), §4.5 (the quantile-sketch primitive). Siblings: `nucleo-frame-spec.md` (the
> frame it ingests), `nucleo-column-spec.md` (the column==tensor-buffer invariant it ingests
> through), `records-spec.md` (the `Table<T>` schema), `nucleo-frame-spec.md` (§9 index
> interface, the zone-map/quantile-sketch it shares a primitive with).
>
> This is a spec. It holds requirements and use cases, the why and the what. Build
> decisions, the how, are deferred as `> **TBD (plan-time):**` markers and collected in §9,
> to be resolved when this spec is turned into a plan. It is outline-numbered for
> addressability.

## 1. Definition

### 1.1 Purpose
`nucleo.trees` is the gradient-boosted decision tree lineage (the XGBoost / LightGBM family)
and the third, independent ML core. It exists to give Cajeta the dominant tabular-ML method as
a fast, self-contained standalone win that rides the columnar substrate and is independent of
the autodiff spine (analysis §3.6, §4.2).

This spec is built around one defining distinction. A gradient-boosted booster's "gradients" are
the closed-form first- and second-order derivatives of the loss evaluated pointwise, the gradient
and Hessian it grows trees against. They are never reverse-mode AD over a compute graph.
Architecturally it shares almost nothing with backprop and never touches autodiff
(no `Grad`, no VJP registry, no `transform-intrinsics-spec.md`). It is off to the side. It is a
faster, self-contained win, lower priority than the DL+graphics spine but cheap.

### 1.2 Scope
- Training a booster from a `Table<T>`. The booster is an additive ensemble of regression
  trees fit to the loss's pointwise gradient and Hessian.
- Zero-copy ingestion from the frame, column-wise and dtype/null-aware, via the
  column==tensor-buffer invariant (`nucleo-column-spec.md` §1.1, `nucleo-frame-spec.md`).
- Histogram binning. Features are quantized up front into a bin-indexed histogram (the
  `QuantileDMatrix` analog) and the booster operates on that compact representation.
- Prediction from a trained booster over a `Table<T>`.
- Feature importance over the trained ensemble.
- Sharing the quantile-sketch primitive with the frame's zone-map / index backlog (one
  primitive, two hats, per analysis §3.6, §4.5).

### 1.3 Non-goals
- **Autodiff of any kind.** No `Grad`, no VJP rules, no eager tape. The grad/Hessian here are
  closed-form loss derivatives, computed analytically per objective, and the autograd engine
  never differentiates them. This is the load-bearing architectural separation
  (`transform-intrinsics-spec.md` / `nucleo-autograd-spec.md` are not dependencies).
- **A working store over the frame.** The dataframe is an ingestion source and never a working
  store. Its job ends at ingestion, and the booster operates on the bin-indexed histogram
  thereafter (§4). Relational ops, lazy chains and joins belong to `nucleo-frame-spec.md`.
- **The broader scikit-learn estimator surface** (clustering, kNN/SVM/NB, PCA). Those are the
  tabular-ML cousin (analysis §3.6 note) and ride the second-tier dataframe path. Only the
  gradient-boosting lineage is in scope here.
- **A faithful XGBoost API clone.** The façade that wraps this into a scikit/xgboost-shaped skin
  is separate. This spec is the núcleo core (the booster, ingest, histogram, predict).
- **Distributed / out-of-core training.** v1 is in-memory, single-process over a `Table<T>`.

### 1.4 Relationship to existing constructs
- **The frame** (`nucleo-frame-spec.md`). A booster trains from a `Table<T>`, consuming it as
  an ingestion source. It does not extend or embed the dataframe engine.
- **The column** (`nucleo-column-spec.md`). Ingestion is column-wise and zero-copy by the
  column==tensor-buffer invariant. A non-null numeric column is the tensor buffer the histogram
  builder reads, with no marshalling, and nullability is consulted via the column's validity
  bitmap.
- **Records** (`records-spec.md`). The `Table<T>` schema names the feature columns, and the
  label column and feature columns are typed fields.
- **The quantile-sketch** (`nucleo-frame-spec.md` §9 / analysis §4.5). The booster's
  per-feature quantile binning is the same primitive as the frame's zone-map / quantile-sketch.
  It is built once and used as a histogram binner here and as an index/zone-map there (one
  primitive, two hats).
- **`cajeta.xpu`**. Histogram construction and split-finding are parallelizable over the
  bin-indexed representation, and device lowering aligns with the device model where it pays.
  That is a plan-time concern and never a v1 commitment.

## 2. Train a booster from a Table

A booster is fit additively. At each round, the loss's pointwise gradient and Hessian are
computed against the current predictions, and a regression tree is grown to reduce the loss.
```cajeta
// schema names the feature + label columns
Table<Sample> data = Table.scanParquet<Sample>("train.parquet");

var booster = GradientBoosting
    .objective(SquaredError)
    .fit(data, label: col.target);
```

**Use cases**
- **2.1** As a developer, when I fit a booster from a `Table<T>` naming a label column and
  feature columns, then I get a trained additive ensemble of regression trees. This is the
  standard gradient-boosting fit, with no autodiff involved.
- **2.2** As a developer, when training proceeds round by round, then each round computes the
  closed-form gradient and Hessian of the chosen objective at the current predictions
  (pointwise, per row) and grows a tree against them, never reverse-mode AD.
- **2.3** As a developer, when I choose an objective (e.g. squared-error regression, logistic
  classification), then its gradient and Hessian are the analytic derivatives of that loss,
  selected per objective. They form a closed-form rule set rather than a differentiated graph.
- **2.4** As a developer, when I set training controls (number of rounds, learning rate / shrinkage,
  max depth, regularization), then they parameterize the fit with the familiar
  gradient-boosting knobs.

> **TBD (plan-time):** [T1] The tree-construction algorithm scope for v1. This covers depth-wise
> vs. leaf-wise growth, the split-gain formulation (the XGBoost gain with L1/L2 regularization),
> how splits are found over the histogram (§4), and the exact set of training controls in v1.

## 3. Zero-copy ingestion from the frame

Ingestion is the only place the dataframe touches the booster, and it involves zero marshalling.
The booster reads feature columns directly as buffers, dtype- and null-aware, by the
column==tensor-buffer invariant.

**Use cases**
- **3.1** As a developer, when I ingest a `Table<T>`, then each feature column is read
  column-wise as its contiguous buffer. The booster iterates features rather than rows,
  matching the columnar layout (`nucleo-column-spec.md`).
- **3.2** As a developer, when a feature column is a non-null numeric type, then it is consumed
  zero-copy. The column buffer is the tensor buffer the histogram builder reads, with no
  conversion or copy (the column==tensor-buffer invariant, analysis §2.3 / §3.6).
- **3.3** As a developer, when a feature column is nullable (`Column<T?>`), then ingestion is
  null-aware. Missing values are consulted via the validity bitmap and handled by the
  booster's missing-value policy as a real absence rather than NaN-as-missing, and they are
  never silently coerced.
- **3.4** As a developer, when ingestion completes, then the dataframe's job ends. The
  booster operates on the bin-indexed histogram (§4) thereafter rather than on the live table.
  The frame is an ingestion source and never a working store (analysis §3.6).
- **3.5** As a developer, when feature columns have heterogeneous dtypes
  (`float32`/`float64`/`int32`/…), then ingestion is dtype-aware and binning normalizes them
  into the common bin-indexed representation (§4).

> **TBD (plan-time):** [T2] The missing-value policy, meaning how nulls are routed at a split
> (the XGBoost default-direction learning, vs. an imputation, vs. a dedicated missing bin), and
> whether categorical features are admitted in v1 or only numeric (categoricals need a distinct
> binning/encoding path).

## 4. Histogram binning — the QuantileDMatrix

The booster quantizes each feature up front into a bin-indexed histogram. Per-feature
quantile boundaries map raw values to small integer bin indices, and the booster operates on that
compact representation (the `QuantileDMatrix` analog).

**Use cases**
- **4.1** As a developer, when I build the booster's working representation, then each feature is
  quantized into bins by per-feature quantile boundaries. Raw values become small integer
  bin indices, materialized once up front.
- **4.2** As a developer, when training searches for splits, then it operates on the
  bin-indexed histogram (gradient/Hessian sums per bin) rather than on raw feature values. The
  compact representation is what makes boosting fast.
- **4.3** As a núcleo author, when I bin features, then the per-feature quantile sketch is the
  same primitive as the frame's zone-map / quantile-sketch (`nucleo-frame-spec.md` §9, analysis
  §4.5). One quantile-sketch implementation serves two consumers, the histogram binner here and
  the index/zone-map there.
- **4.4** As a developer, when bins are constructed, then the bin count (histogram resolution) is
  a control. More bins give finer splits at higher memory/compute cost, and the quantile
  boundaries are computed over the ingested column (zero-copy from §3).

> **TBD (plan-time):** [T3] The histogram method. This covers exact (sort-based) vs. approximate
> (quantile-sketch) split finding, the sketch algorithm (GK / t-digest / weighted quantile
> sketch, which must be the *same* one the index backlog adopts so the primitive is genuinely
> shared), the default bin count, and whether histograms are recomputed per node or subtracted
> (the parent-minus-sibling histogram trick).

## 5. Predict from a booster

**Use cases**
- **5.1** As a developer, when I call `booster.predict(data)` over a `Table<T>`, then each row is
  routed through every tree in the ensemble and the per-tree leaf values are summed (plus the
  base score) into a prediction. This is the standard additive inference.
- **5.2** As a developer, when I predict, then ingestion of the prediction table is the same
  zero-copy, dtype/null-aware column read as training (§3). The prediction path consumes the
  frame identically.
- **5.3** As a developer, when the objective is classification, then prediction applies the
  objective's link/inverse-link (e.g. logistic) to produce calibrated outputs, consistent with
  the objective chosen at fit (§2.3).

> **TBD (plan-time):** [T4] Whether predict requires re-binning the input to the training bin
> edges. It should route on raw thresholds the tree stores, so no. The split representation
> the tree stores, raw-threshold vs. bin-index, is a plan-time decision that determines this.

## 6. Feature importance

**Use cases**
- **6.1** As a developer, when I ask a trained booster for feature importance, then I get a score
  per feature column over the ensemble (e.g. split-count / gain / cover), keyed back to the
  schema's named columns (`records-spec.md`) so the result is typed rather than positional.

> **TBD (plan-time):** [T5] Which importance measures ship in v1 (weight / gain / cover) and the
> default, and whether SHAP-style per-prediction attributions are in scope or deferred.

## 7. Independence from the autodiff spine

This section is normative about what `nucleo.trees` does not depend on. The architectural
separation is a feature rather than an omission.

**Use cases**
- **7.1** As a núcleo author, when I build `nucleo.trees`, then it depends on `nucleo.column` /
  `nucleo.frame` (ingest) and the shared quantile-sketch. It does not depend on `nucleo.autograd`
  or the transform intrinsics, so the lineage is self-contained.
- **7.2** As a maintainer, when I read the booster's gradient/Hessian computation, then it is a
  closed-form, per-objective analytic routine (one routine per objective), with no call into
  the VJP registry or any autodiff machinery. The "gradient" is a loss derivative rather than a
  backprop.
- **7.3** As a developer, when I use `nucleo.trees`, then I can do so without any DL/graphics
  surface present. It is a standalone tabular-ML win that needs only the columnar substrate.

## 8. Acceptance criteria (spec-level)
- A booster trains from a `Table<T>` (named label + feature columns) into an additive
  regression-tree ensemble, fit against the objective's closed-form pointwise gradient and
  Hessian, with no call into the autograd engine or the VJP registry.
- Ingestion from the frame is column-wise, dtype-aware, null-aware, and zero-copy for non-null
  numeric columns (the column==tensor-buffer invariant). The frame's role ends at ingestion.
- Features are quantized into a bin-indexed histogram (`QuantileDMatrix` analog), and training
  searches splits over the histogram.
- The per-feature quantile binning is the same primitive as the frame's zone-map /
  quantile-sketch (one primitive, two hats).
- `predict` over a `Table<T>` produces additive ensemble predictions via the same zero-copy
  ingest.
- Feature importance is reported per named feature column.
- `nucleo.trees` is self-contained. It depends on column/frame and the quantile-sketch, and
  it does not depend on the autodiff spine.

## 9. Open questions (resolve at plan time)
- **[T1]** Tree-construction algorithm scope: depth-wise vs. leaf-wise growth, the split-gain
  formulation, the v1 training controls (§2).
- **[T2]** Missing-value policy (default-direction learning vs. imputation vs. missing bin) and
  whether categorical features are admitted in v1 (§3).
- **[T3]** Histogram method: exact vs. approximate split finding, the sketch algorithm (which must
  be the *same* one the index backlog adopts), default bin count, parent-minus-sibling subtraction
  (§4).
- **[T4]** Whether the tree stores raw thresholds or bin indices (determines whether predict
  re-bins) (§5).
- **[T5]** Feature-importance measures in v1 (weight/gain/cover) and whether SHAP-style
  attributions are in scope (§6).
- **Objective / loss set in v1**: which objectives ship (squared-error regression, logistic
  binary classification, softmax multiclass, …) and the closed-form grad/Hessian for each (§2.3).
