# table-fit — dataframe-to-estimator in two lines (loader materialization + the ml bridge)

## 1. Definition

Close the last gap between the dataframe and the estimator ecosystem: a
`Table<R>` loaded from Parquet/Arrow (and, once unblocked, CSV) becomes an
estimator fit in **two lines**, for every `dev.cajeta.ml.Predictor` (linear
models and the xgboost adapter alike) — the cajeta-ml spec's deferred use
case §3.1.

The gate is a compiler defect family, not a design gap: **synthesis reads
types that are name-bound but not yet materialized.** Two members, verified
2026-07-30 on the v0.12.0 tree (`test/nucleo/TableLoaderMaterializationTests.cpp`):

- **B (live, reproduced):** a `record R` declared in a **separate file** from
  the first `Table<R>` use fails the schema synthesizer's record check with
  `CAJETA_ERROR_FRAME_SCHEMA: 'R' is not a record` — the cross-file record's
  class exists by name but its record flag/properties are not yet populated
  when member synthesis runs. Single-file always worked; every real consumer
  (user records in their own files) hits this immediately.
- **A (previously SIGSEGV, now latent):** member-synthesis imports (`DynCol`,
  …) are injected by `injectImportIfUnbound` as **name-only** bindings —
  nothing loads the class. In 2026-07-23 trees this SIGSEGV'd the compiler on
  the first `Table<R>` instantiation inside a template-static body (what
  blocked `Table.fromCsv<R>`, nucleo-frame 16.1.1). The direct repro of that
  shape now **passes** on v0.12.0; the injection is still name-only, so the
  gap is pinned by regression tests and the injection sites are hardened to
  force-load, with `fromCsv` resurrection as the proof on the original path.

## 2. Feature: compiler — materialize before synthesis reads

Any type a synthesizer inspects (schema record flags, properties) or a
synthesized body references (member-synthesis imports) must be **materialized
— loaded with flags and properties populated — not merely name-bound**, at
the point of first use, regardless of which compilation unit declares it or
whether the use site is a template-static body.

Use cases:
- 2.1 Cross-file record, direct: `record Rec` in `Rec.cajeta`, first
  `Table<Rec>` in `D.cajeta` — compiles and runs; accessors typed and correct.
- 2.2 Cross-file record through a template-static body (`wrap<R>` performing
  the program's first `Table<R>` instantiation) — compiles and runs.
- 2.3 The schema contract is not softened: a cross-file **non-record**
  argument still fails with `CAJETA_ERROR_FRAME_SCHEMA`, same message.
- 2.4 Template-static first instantiation (single-file) stays green — the
  former SIGSEGV shape is regression-pinned.
- 2.5 Member-synthesis import injection force-loads the imported class (the
  injection sites gain materialization, with a `CAJETA_DBG_RESOLVE`-style
  trace when it fires).

## 3. Feature: nucleo-frame — `fromCsv<R>` resurrected (16.1.1)

The blocked CSV boundary rides the fix: `Table.fromCsv<R>` (a template
static whose body performs the first `Table<R>` instantiation) parses typed
columns via the already-designed CsvReader mapping. This is both a shipped
feature and the acceptance proof that gap A is dead on its original path.

Use cases:
- 3.1 `fromCsv<Tick>` parses typed columns (header→field mapping, physical
  types per the schema record); values verified against a fixture string.
- 3.2 Malformed rows / arity mismatch fail loudly (`FrameException`), not as
  silent nulls.

## 4. Feature: cajeta-ml — the two-line fit (`Frames`)

A generic bridge in `dev.cajeta.ml` from any `Table<R>` to the design
matrix, using the synthesized generic introspection surface
(`colCount`/`colNameAt`/`colTypeAt`) + `f64At`/`rowCount` — so it works for
every schema record with no per-class overloads, and feeds **every**
`Predictor`:

```cajeta
Design d = Frames.design<Tick>(t, "price");   // features = float64 cols in schema order, minus the target
est.fit(d.x, d.y);                            // any Predictor: LinearRegression, XGBRegressor, …
```

Use cases:
- 4.1 Two-line fit from an Arrow/Parquet-loaded table (spec §3.1 of
  cajeta-ml): `Frames.design<R>(t, target)` → `Design { x (n,p), y (n,) }`.
- 4.2 Column selection is explicit and auditable: features are the
  float64-physical columns in schema order minus the target; `Design`
  records `featureNames` so a model summary can name coefficients.
- 4.3 Explicit-features overload `Frames.design<R>(t, features, target)` for
  subsetting/reordering.
- 4.4 Loud failures: unknown target/feature name, non-float64 target,
  zero features, and null-bearing selected columns throw `MlException`
  (fill or drop nulls first — the frame owns imputation).
- 4.5 The bridge works identically for the xgboost adapter (`XGBRegressor`)
  — proof that the protocol + bridge compose across libraries.

## 5. Toolchain lifecycle

The ml-side tests place records in their own files (the natural shape), so
Feature 4 **requires a released toolchain carrying Feature 2**. The cut is
v0.12.1 (compiler fix + fromCsv; no API surface changes elsewhere), swept at
the release gate per policy, then the ml/xgboost CI pins bump.

## 6. Non-goals

- Categorical/string feature encoding (one-hot etc.) — a future
  preprocessing feature; the bridge takes float64 columns only.
- Lazy-plan pushdown into the design matrix (collect() first).
- `fit(Table<R>)` overloads on estimator classes — the bridge + protocol
  compose better than N overloads (decided here; supersedes the §3.1
  sketch's phrasing).
