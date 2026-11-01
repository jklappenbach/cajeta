# pandas Façade — Specification

> Status: draft for review (2026-06-23). This is a Layer-2 façade (`dev.cajeta.pandas`), an
> optional, thin recognizability skin over the Polars-shaped `nucleo.frame`. It is
> explicitly demoted. The Polars-shaped frame is the primary, recommended API, and this
> skin exists only to give migrating pandas users a familiar door (`read_parquet`,
> `groupby`, `merge`, `pivot`, `resample`) that maps onto the better frame idioms.
> Companion analysis: `python-stack-analysis.md` §3.5 (the most aggressive correction of
> any library, which goes Polars/Arrow instead of classic pandas) and §6 ("pandas is an
> optional skin"). Design context: `target-experience.md` §3 (the recommended frame
> surface). Engine cross-refs: `nucleo-frame-spec.md` (the lazy, typed, schema-in-the-type
> dataframe, of which this façade re-specifies nothing), `nucleo-column-spec.md` (the
> nullable column substrate), `records-spec.md` (the schema-as-record machinery
> `Table<Tick>` stands on).
>
> This is a spec. It holds requirements and use cases, the why and the what. Build
> decisions, the how, are deferred as `> **TBD (plan-time):**` markers and collected in
> §10, to be resolved when this spec is turned into a plan. It is outline-numbered for
> addressability.

## 1. Definition

### 1.1 Purpose
`dev.cajeta.pandas` is a migration aid rather than a recommended surface. Its entire job is to
let a developer carrying pandas muscle memory (`pd.read_csv`, `df.groupby(...).agg(...)`,
`pd.merge`, `df.pivot_table`, `df.resample`) land on cajeta without first learning the
Polars-shaped API, while the skin steers them toward the better frame idioms underneath.
Every familiar name maps onto a `nucleo.frame` operation, and the frame is doing the work. The
skin is recognizable rather than faithful (analysis §1.3, §2.5). Where classic pandas encodes a
genuine mistake (implicit index alignment, `inplace=`, object dtype, NaN-as-missing), the skin
does not reproduce it (analysis §3.5 drop list).

### 1.2 Scope
- A thin set of static methods (Cajeta has no global functions) mirroring the
  `pd.*` entry points a migrating user reaches for first: `Pandas.readCsv`,
  `Pandas.readParquet`, `Pandas.merge`, `Pandas.concat`.
- A thin method skin mirroring the most-used `DataFrame.*` verbs, namely `groupby`/`agg`,
  `merge`/`join`, `pivot`/`melt`, `resample`, `sortValues` and `dropna`/`fillna`. Each
  delegates to the corresponding `nucleo.frame` operation (`nucleo-frame-spec.md`).
- Steering. Where a pandas idiom has a strictly better frame idiom, the skin's
  documentation, return types, and (where helpful) deprecation-style guidance point the
  user at the frame API (§7).
- A clear demotion contract (§2). This façade is opt-in, secondary, and labelled as a
  migration aid everywhere it surfaces.

### 1.3 Non-goals
- **Being the recommended API.** The Polars-shaped `nucleo.frame` (`target-experience.md`
  §3) is primary. This skin is for migration only and must say so (§2).
- **Re-specifying the frame engine.** Lazy evaluation, the expression graph, the query
  optimizer, columnar storage, nullable types, joins, groupby-aggregate, and resampling are
  owned by `nucleo-frame-spec.md` / `nucleo-column-spec.md`. This façade only renames them
  and re-shapes calls onto them.
- **Bug-for-bug pandas fidelity.** The `Index`/`MultiIndex` and its implicit alignment,
  `inplace=`, the `object` dtype zoo, NaN-as-missing, and the copy/view ambiguity
  (`SettingWithCopyWarning`) are dropped and never reproduced (analysis §3.5).
- **The full pandas surface.** Only the high-traffic migration verbs are in scope. The long
  tail of pandas methods is out, and a user reaching past the skin is steered to the frame API.

### 1.4 Relationship to existing constructs
- It is a Layer-2 sibling of `dev.cajeta.scipy` / `.torch` / `.keras` (analysis §4.3), but
  it is uniquely demoted. The others are the primary surface for their domain, and this one
  is not.
- It sits entirely on top of `nucleo.frame`. Every façade call lowers to one or more frame
  operations (`nucleo-frame-spec.md`), and the façade holds no engine state.
- Schemas remain records (`records-spec.md`). A pandas-skin `DataFrame` is still a typed
  `Table<Schema>` underneath, and the skin does not reintroduce a dynamic, schemaless frame.
- Missing data is a real nullable column (`Column<T?>`, `nucleo-column-spec.md`), never
  a NaN sentinel. So `dropna`/`fillna` operate on a validity bitmap rather than on
  NaN-poisoned floats (analysis §3.5).

> **TBD (plan-time):** [P1] Whether the skin's `DataFrame` is (a) a distinct wrapper type
> around `Table<Schema>`, or (b) just extension methods named the pandas way directly on the
> frame type. Lean: (b), extension methods, so there is no second frame type to keep in
> sync and migration is a one-line rename rather than a type change.

## 2. The demotion contract — migration aid, not the surface

**Use cases**
- **2.1** As a new cajeta user reading the docs, when I look for the recommended dataframe
  API, then I am pointed at the Polars-shaped `nucleo.frame` (`target-experience.md`
  §3). The pandas façade is presented as an optional migration layer and never as the default.
- **2.2** As a migrating pandas user, when I import `dev.cajeta.pandas`, then it is clearly
  labelled a recognizability skin, and the obvious next step (the frame idiom it maps to) is
  visible at the call site or in its result type (§7).
- **2.3** As a maintainer, when a pandas verb has no sound mapping because it depends on a
  dropped mistake (implicit index alignment, `inplace=`), then the skin does not provide it.
  Attempting it is a compile-time absence that steers the user to the frame API, and never a
  silent wrong-result reproduction (§4, analysis §3.5).

## 3. IO entry points — read_csv / read_parquet (→ frame scan)

**Use cases**
- **3.1** As a pandas user, when I call `Pandas.readCsv("ticks.csv")`, then it maps to a
  frame scan (`Table.scanCsv` / `Table.scanParquet`, per `target-experience.md` §3 and
  `nucleo-frame-spec.md`) returning a typed, lazy frame. Eager materialization is opt-in
  rather than the default, which corrects pandas' eager-read habit additively.
- **3.2** As a pandas user, when I call `Pandas.readParquet("ticks.parquet")` with a known
  schema (`readParquet<Tick>(...)`), then the schema is a record in the type
  (`Table<Tick>`, `records-spec.md`). A column typo is a compile error rather than a runtime
  `KeyError` (analysis §3.5 "the distinctive version").
- **3.3** As a developer, when I read a file with missing values, then absent cells become a
  nullable column (`Column<T?>`), never a NaN-sentinel `object` column (analysis §3.5
  drop list).

## 4. The dropped mistakes — what the skin refuses

**Use cases**
- **4.1** As a developer, when I expect pandas' implicit `Index`/`MultiIndex` alignment on
  an arithmetic or join, then the skin does not provide implicit alignment. Joins are
  explicit key joins (§6) and there is no row-label index (analysis §3.5).
- **4.2** As a developer, when I reach for `inplace=True`, then the skin has no `inplace`
  parameter. Every operation returns a new frame. Polars eliminated mutation, so the
  copy/view ambiguity and `SettingWithCopyWarning` are gone (analysis §3.5).
- **4.3** As a developer, when I expect an `object` dtype catch-all, then there is none.
  Every column is a real typed (optionally nullable) column (`nucleo-column-spec.md`).
- **4.4** As a developer, when I treat NaN as "missing", then missingness is a validity
  bitmap on a nullable column, and `NaN` (when present in a float column) is a genuine
  floating-point value, distinct from null (analysis §3.5).

## 5. groupby / agg (→ frame groupBy)

**Use cases**
- **5.1** As a pandas user, when I write `df.groupby("venue").agg(...)`, then the skin maps
  it to the frame's `groupBy(col.venue).agg(...)` (`target-experience.md` §3,
  `nucleo-frame-spec.md`). It is the same lazy, expression-based, optimizer-planned aggregation.
- **5.2** As a developer, when I group on multiple keys, then the skin accepts the familiar
  pandas spelling and lowers it to the frame's multi-key `groupBy`.
- **5.3** As a developer, when I write a typed aggregation, then column references are
  typed field accesses (`col.price`, where a typo is a compile error), and the skin steers me
  from string column names toward the typed `col.*` expressions (§7).

## 6. merge / join (→ frame join)

**Use cases**
- **6.1** As a pandas user, when I call `Pandas.merge(left, right, on:, how:)`, then it maps
  to the frame's typed `join` (`nucleo-frame-spec.md`). It is an explicit key join, never an
  implicit index alignment (§4.1).
- **6.2** As a developer, when I specify join keys and a join kind (`how: Inner/Left/...`),
  then they are named, typed arguments, and a missing or mistyped key is a compile-time
  error rather than a silent empty result.
- **6.3** As a developer, when I `concat` frames (`Pandas.concat([a, b])`), then it maps to
  the frame's vertical/horizontal concatenation with schema compatibility checked at compile
  time where the schemas are records.

## 7. pivot / melt / resample, and steering toward frame idioms

**Use cases**
- **7.1** As a pandas user, when I call `pivot`/`pivotTable` or `melt`, then the skin maps
  them to the frame's reshape operations (`nucleo-frame-spec.md`) with named arguments
  replacing pandas' positional+`values=`/`index=`/`columns=` sprawl.
- **7.2** As a pandas user, when I call `df.resample("1min").agg(...)`, then it maps to the
  frame's time-series resample (`ticks.resample(col.ts, every: 1.minutes).agg(...)`,
  `target-experience.md` §3). Analysis §3.5 calls resampling "genuinely valuable, poorly
  served elsewhere".
- **7.3** As a migrating user, when I use a skin verb that has a strictly better frame idiom
  (lazy chaining, `col.*` expressions, pushdown filters), then the skin steers me toward
  it through the result type, documentation, and migration guidance. The skin is a ramp
  off itself and never a destination (§2).

> **TBD (plan-time):** [P2] The steering mechanism. Candidates are a `.lazy()` / `.toFrame()`
> escape that hands back the native frame, compiler deprecation-style hints on skin methods,
> doc-only guidance, or a combination. Lean: a one-call `.toFrame()` escape plus doc guidance,
> with no noisy per-call warnings.

## 8. Eager-vs-lazy and materialization

**Use cases**
- **8.1** As a pandas user expecting eager dataframes, when I use the skin, then the frame is
  lazy by default, so the optimizer plans the chain before executing
  (`target-experience.md` §3). The skin makes `collect()`/eager materialization explicit and
  recognizable rather than running each call eagerly.
- **8.2** As a migrating user, when I need pandas' eager feel during migration, then an eager
  convenience exists, clearly marked as the slower path, with the lazy frame as the
  recommended target.

> **TBD (plan-time):** [P3] How much pandas eager-feel to emulate during migration. The ends
> are fully eager skin methods (each returns a materialized frame) and lazy-by-default with an
> explicit `collect()`. Lean: lazy-by-default, which is honest about the engine, with a
> documented eager helper for migration only.

## 9. Acceptance criteria (spec-level)
- The façade is demoted in every surface (docs, naming, result types). The Polars-shaped
  `nucleo.frame` is the recommended API, and this skin is labelled a migration aid (§2).
- Familiar entry points (`readCsv`/`readParquet`/`merge`/`concat`) and verbs
  (`groupby`/`agg`/`merge`/`pivot`/`melt`/`resample`) map cleanly onto `nucleo.frame`
  operations and re-specify no engine internals.
- The dropped pandas mistakes are absent: no `Index`/`MultiIndex`
  implicit alignment, no `inplace=`, no `object` dtype, no NaN-as-missing (§4).
- Missing data is a real nullable column. Schemas remain records with compile-time
  column typing.
- Entry points are static methods, verbs are methods, and all take named arguments. There are
  no global functions and no global state.
- The skin steers toward the better frame idioms (§7) rather than entrenching pandas
  habits.

## 10. Open questions (resolve at plan time)
- **[P1]** Distinct `DataFrame` wrapper type vs. pandas-named extension methods on the frame
  type (§1.4).
- **[P2]** The steering mechanism toward the native frame (§7).
- **[P3]** Eager-vs-lazy migration ergonomics (§8).
- Exactly which pandas verbs are in the v1 high-traffic set vs. deferred to "use the frame
  API", resolved against `nucleo-frame-spec.md`'s operation set at plan time.
- Whether the skin ships at all in v1 or is deferred until the native frame surface has
  proven itself (the analysis marks it *optional*, §3.5).
