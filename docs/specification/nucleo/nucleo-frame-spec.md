# Núcleo Frame — Specification

> Status: draft for review (2026-06-23). This is the Polars-shaped lazy typed dataframe,
> núcleo's tabular surface (`dev.cajeta.nucleo.frame`), and it includes the pluggable index
> interface. Layer-1b. The companion analysis is `python-stack-analysis.md` §3.5 (pandas to
> Polars) and §4.5 (indexing). Its siblings are `nucleo-column-spec.md` (the columnar buffer it
> stores over), `nucleo-expr-spec.md` (the lazy expression engine it plans on),
> `records-spec.md` (the record that is the schema), and `source-synthesis-spec.md` (the
> facility that synthesizes typed column accessors). The design context is `target-experience.md`
> §3.
>
> This is a spec, so it states requirements and use cases, the why and the what. Build decisions,
> the how, are deferred as `> **TBD (plan-time):**` markers and collected in §12, to be
> resolved when this spec is turned into a plan. It is outline-numbered for addressability.

## 1. Definition

### 1.1 Purpose
A Table is a typed, lazy, expression-planned dataframe. It is a set of named columns whose
schema lives in the type (`Table<Tick>`, where `Tick` is a record). Relational operations over
it (`filter`, `select`, `groupBy`, `agg`, `sort`, `join`, window/rolling, pivot/melt,
time-series resample) build a plan that a query optimizer rewrites before it executes. It
exists to give Cajeta the tabular abstraction of heterogeneous named columns with relational
operations, which is genuinely fundamental. It does so without porting pandas' specific
DataFrame, which is not fundamental (analysis §3.5). The reference shape is Polars (lazy, no
index, expression-based, query-optimized) and DuckDB (logical plan → optimizer → vectorized
engine), over Arrow bytes (`nucleo-column-spec.md`).

The schema is in the type, and only a typed language can make that move. Column names and
types are known at compile time, so every `KeyError` and silent dtype coercion becomes a
compile error and `ticks.price` autocompletes (analysis §3.5). Haskell's *Frames* proves the
idea. Nobody has done it in a language aimed at scientific users.

### 1.2 Scope
- A typed dataframe `Table<T>` whose column set is derived at compile time from the
  record `T`'s fields (`nucleo-column-spec.md` SoA: one `Column` per field).
- A lazy, expression-based relational API, with `filter`, `select`/`with`, `groupBy`/`agg`,
  `sort`, `join`, window/`rolling`, `pivot`/`melt`, and an explicit terminal `collect()`. It
  builds a plan on the expression engine (`nucleo-expr-spec.md`) and forces only at the
  terminal.
- Query optimization at the frame level, which is predicate pushdown and projection
  pushdown to the scan. The engine performs the rewrites, and the frame exposes the plan.
- Real nullable types (`Column<T?>`) as the missing-data model. NaN is not the missing value.
- Typed column access (`ticks.price`) via source-synthesized accessors
  (`source-synthesis-spec.md`). A typo is a compile error.
- Time-series resample, the one pandas feature genuinely worth porting.
- The pluggable index interface (§9). It is one contract a column advertises and a query asks
  for, and every concrete index is a deferrable backlog behind it. Indexes are bulk-loaded,
  because Arrow batches are immutable.
- Arrow C-Data interop at the import/export boundary, `scanParquet` in and `exportArrow`
  out, delegated to `nucleo-column-spec.md`.

### 1.3 Non-goals
- **The columnar buffer itself.** Physical layout, validity bitmaps, and Arrow interchange live
  in `nucleo-column-spec.md`. This spec stores over columns and does not define them.
- **The expression IR, the optimizer passes, fusion, lowering.** Those are `nucleo-expr-spec.md`.
  This spec defines the relational surface those passes plan and run. It does not define the
  engine.
- **The pandas mistakes, dropped on purpose** (analysis §3.5). These are Index / MultiIndex and
  their implicit alignment, `inplace=`, the object-dtype zoo, NaN-as-missing, and
  the copy/view ambiguity with its `SettingWithCopyWarning` (Polars eliminated mutation
  entirely). None of these are ports. They are explicit refusals.
- **Concrete index implementations.** The interface (§9) is the only commitment. Zone-maps,
  in-memory B+, and Z-order-over-B+ are a near-term backlog. R-tree / BVH / HNSW are deferred
  and correctly placed (§9.4), and none of them is specified here as a built component.
- **A cost-based query optimizer / statistics-driven join reordering.** v1 planning is the
  rule-based rewrites the engine commits to (predicate + projection pushdown). Cost-based
  planning is deferred (`nucleo-expr-spec.md` [X4]).
- **Cross-machine / distributed execution.** Execution is single-process, on the engine's
  CPU-or-GPU target.
- **An eager pandas-style `DataFrame`.** `Table` is lazy-first. The optional
  `dev.cajeta.pandas` recognizability skin (analysis §3.5) is a separate façade and is outside
  this spec.

### 1.4 Relationship to existing constructs
- **Records are the schema.** `Table<Tick>` derives its columns from `Tick`'s fields by
  compile-time reflection (`records-spec.md` §6): column `price` has type `float64`, etc. A
  record is the type-level descriptor. The table's physical store is one `Column<T>` per
  field (`records-spec.md` §7, SoA), and it is never an array of record instances.
- **Columns are the storage.** Each field lands on a `Column<T>` (non-null) or `Column<T?>`
  (nullable) per `nucleo-column-spec.md`. A non-null numeric column is bit-identical to a
  tensor buffer, so the same engine computes over table columns and tensors.
- **The expression engine is the planner.** `col.price > 0.0`, `(col.price * col.size).sum()`,
  and the `filter → groupBy → agg → sort` chain all build nodes in the same lazy graph the
  tensor engine uses (`nucleo-expr-spec.md` §2/§4). The frame is a relational skin over that
  one engine (analysis §2.2).
- **Source synthesis generates the accessors.** Typed member access `ticks.price` is produced
  by the Tier-A synthesizer keyed on the `Table<T>` instantiation
  (`source-synthesis-spec.md` §1.2: template-instantiation trigger), and it is re-checked like
  hand-written code.
- **Templates are monomorphized.** `Table<Tick>` and `Table<Splat>` are distinct, fully
  typed types with distinct synthesized accessors and distinct column sets. There is no erasure.
- **Named arguments + defaults** (✅ today) carry the keyword API surface
  (`resample(col.ts, every: 1.minutes)`, `join(other, on: ..., how: Left)`).

> **Resolved:** [F-NAME] The primary type is `Table<T>` in module `dev.cajeta.nucleo.frame`.
> It fits the record decision, since a record is a typed row and a `Table<T>` is rows sharing
> the schema `T`, which is the database lineage records already lean on. It collides with
> nothing and carries no pandas baggage. `DataFrame` is reserved as the pandas-façade alias
> (`dev.cajeta.pandas`), so migrants get the familiar name, mapped onto `Table` underneath. The
> module is named for the conceptual area, and the `Table` type living under `…frame` is
> intentional.

## 2. The typed table — schema in the type

A `Table<T>` is parameterized by a record `T` whose fields are the schema. The column set,
each column's name, and each column's type are fixed at compile time.

```cajeta
record Tick { Instant ts; float64 price; float64 size; Symbol venue; }
Table<Tick> ticks = Table.scanParquet<Tick>("ticks.parquet");
```

**Use cases**
- **2.1** As a developer, when I write `Table<Tick>` for a record `Tick`, then the table has
  exactly one column per `Tick` field, and each column's name and type are derived from the
  field at compile time (`records-spec.md` §6.1). There is no runtime schema discovery.
- **2.2** As a developer, when I declare a `Table<T>` where `T` is not a record, then it is
  a compile error. The schema parameter must be a record, the type-level descriptor.
- **2.3** As a developer, when a record field is a nullable type (`float64?`), then the
  corresponding column is `Column<float64?>` and carries a validity bitmap, and a non-null
  field (`float64`) is `Column<float64>` with no bitmap. Nullability is a per-column type fact
  and never a runtime dtype flag (`nucleo-column-spec.md` §3).
- **2.4** As a developer, when I have two tables of the same schema (`Table<Tick>`), then
  they are the same type and combine without re-alignment. Two tables of different schemas
  are different types, and only the operations that bridge schemas (join, select-into-new-record)
  connect them. There is no implicit Index alignment, which is the pandas sin this drops.
- **2.5** As a developer, when I print or inspect an unforced `Table<T>`, then I see its schema
  and its unexecuted plan rather than its rows. Building never executes (`nucleo-expr-spec.md`
  §2.5/§5.1).

> **Resolved:** A `Table<T>` has exactly `T`'s fields as its columns, because the record is the
> table's type. The case of operating on any table that has at least `T`'s columns is expressed
> by the bounded wildcard `Table<? extends T>` (§4.3.3). That works through subtyping, and a
> table never carries columns beyond its schema. The schema stays exact, and wildcards handle
> the superset reads.

## 3. Typed column access — the typo is a compile error

A table exposes its columns in two ways, and both are checked at compile time against the
schema. One is the typed member accessor (`ticks.price`). The other is the column-expression
builder (`col.price`). Either way, a name not in the schema fails to compile. It is never a
runtime `KeyError`.

**Use cases**
- **3.1** As a developer, when I read a typed member accessor (`ticks.price`), then it resolves
  to the `float64` column for that field. A typo (`ticks.prce`) is a compile error rather than
  a runtime lookup (analysis §3.5; target-experience §3). The accessor is generated by the
  source-synthesis facility keyed on the `Table<T>` instantiation
  (`source-synthesis-spec.md`).
- **3.2** As a developer, when I build a column expression in a relational operation
  (`filter(col.price > 0.0)`), then `col.price` builds a typed column-reference node in the
  expression graph (`nucleo-expr-spec.md` §2.2). A name not in the schema is a compile error,
  and the comparison's operand type is the field's type, so a `col.venue > 0.0` type mismatch is
  a compile error too.
- **3.3** As a developer, when I read a whole-column scalar through the member accessor
  (`ticks.price.max()`), then it is a typed column operation whose result type follows from the
  field type, and a typo anywhere in the chain is a compile error
  (`var spread = ticks.price.max() - ticks.price.min();`, target-experience §3).
- **3.4** As a developer, when I reference a column that the schema declares nullable
  (`ticks.lastTrade` of `float64?`), then its accessor is typed nullable and operations on it
  carry null-aware semantics (§7). A non-null column's accessor carries no nullability branch
  (`nucleo-column-spec.md` §3.3).

> **Resolved:** [F7] Both, with distinct, non-redundant roles over the same typed columns:
> - `col.price` is the typed column-expression builder. It is late-bound to a relational op's
>   input schema and used inside transforms (`filter(col.price > 0.0)`, `agg(col.price.sum())`).
> - `ticks.price` is typed member access bound to a specific table. It is used for direct or
>   terminal access on that table, such as a scalar reduction `ticks.price.max()` or extracting
>   a column.
>
> Both are schema-checked, so a typo is a compile error on a typed `Table<T>`, and both are
> generated by the one source-synthesis synthesizer keyed on `Table<T>`. On a `Table<?>` neither
> typed form is available, so you use the dynamic `col("price")` (§4.3.2). Resolved jointly with
> `records-spec.md` [F7].

## 4. The lazy relational API

Relational operations build a plan. Each composes a node onto the plan, and nothing executes
until an explicit terminal forces it. This is the Polars/DuckDB model (analysis §3.5), and it
rides the engine's lazy graph directly (`nucleo-expr-spec.md` §2).

```cajeta
var vwap = ticks
    .filter(col.price > 0.0)                       // predicate node, pushed to scan
    .groupBy(col.venue)
    .agg( (col.price * col.size).sum() / col.size.sum() as "vwap" )
    .sort(col.vwap, descending: true)
    .collect();                                    // nothing ran until here
```

**Use cases**
- **4.1** As a developer, when I call a relational operation (`filter`, `select`/`with`,
  `groupBy`, `agg`, `sort`, `join`, `rolling`, `pivot`/`melt`, `resample`), then it returns a
  new lazy `Table`/plan node. No rows are read or materialized at the call
  (`nucleo-expr-spec.md` §5.1), and the original table is unchanged. There is no `inplace=` and
  no copy/view ambiguity, which are the pandas sins this drops.
- **4.2** As a developer, when I chain operations
  (`.filter(...).groupBy(...).agg(...).sort(...)`), then the chain accumulates into a single
  plan whose root is the final operation, with earlier operations as inputs
  (`nucleo-expr-spec.md` §2.3).
- **4.3** As a developer, when I `select`/`with`/`groupBy`+`agg`/`join` to a differently-shaped
  result, then the precise nominal schema is no longer statically known, so the result is a
  schema-erased `Table<?>`. The erasure is visible in the type as the `?`. The
  relational plan still tracks the real columns for force-time validation, but the static
  surface is `Table<?>` until re-bound. There are three ways to proceed, and all are ordinary
  language features:
  - **4.3.1 (narrow)** As a developer, when I `.as<VenueVwap>()` on a `Table<?>`, then it narrows
    to `Table<VenueVwap>`. This is a checked cast. The real (plan/runtime) schema is validated
    against `VenueVwap`'s fields/types, and a mismatch is a clear located error (*"column `vwap`
    absent / has type X; `VenueVwap.vwap` is `float64`"*). It is never a silent wrong type.
  - **4.3.2 (dynamic)** As a developer, when I access a column on a `Table<?>` without narrowing,
    then I use the dynamic accessor `col("vwap")`, which is string-keyed and validated at force
    time. The typed `df.vwap` member is not available on `Table<?>`, and the error says exactly
    that: *"schema not statically known here; narrow with `.as<R>()` or use `col(\"...\")`."*
  - **4.3.3 (covariant read)** As a developer, when I write a function over any table with at
    least `Tick`'s columns, then I type the parameter `Table<? extends Tick>`, the existing
    bounded wildcard. It accepts `Table<Tick>` and `Table<TradeTick>`, with `Tick`'s typed
    accessors available because the bound guarantees them.

> **Resolved (provisional):** derived-schema typing is gradual, via existing wildcards. It is
> typed at the edges and dynamic in the middle. A named source `Table<Tick>` and an explicitly
> re-bound `Table<R>` (`.as<R>()`) are statically typed. A schema-changing op yields a visible
> `Table<?>` that you narrow or access dynamically. There are no anonymous structural records,
> so the records decision holds. There is no magic, since the model is templates plus
> `? extends` plus a checked cast, which are the language's normal rules with no bespoke type
> theory. Errors are diagnosable, since they are ordinary wildcard/cast messages that name the
> fix. The fully-typed-pipeline option, in which the compiler computes a nominal result schema,
> is deferred. It is precisely the path that introduces type-inference "magic" and
> hard-to-diagnose errors we are avoiding, and it stays open only if it can be done without
> them. A code audit on 2026-06-23 confirmed that cajeta supports use-site wildcard variance.
> The grammar accepts `<?>`/`<? extends T>`/`<? super T>` (`CajetaParser.g4`), and
> `CajetaClass::isAssignableToWildcard` makes `Table<TradeTick>` a genuine subtype of
> `Table<? extends Tick>` (tested, `TemplateWildcardP6Tests`). It integrates with
> monomorphization via capture-projection (`CajetaType::captureProject`). `<? extends Tick>`
> projects to `Tick` for member access, so §4.3.3's accessors resolve, while an unbounded `<?>`
> is pointer-only, so §4.3.2 must narrow or use `col("...")`. Cajeta's existing wildcard
> semantics map exactly onto this model. `.as<R>()` is a frame-level checked narrowing that
> validates the runtime schema. It is frame logic and does not depend on the wildcard machinery.
- **4.4** As a developer, when I call `groupBy(keys).agg(exprs)`, then the keys and the
  aggregation expressions build a group-aggregate node. The aggregation expressions are
  column expressions over the engine, so `(col.price * col.size).sum()` is an elementwise fusion
  feeding a reduction (`nucleo-expr-spec.md` §7.2) and never an opaque string operation.
- **4.5** As a developer, when I call `.collect()`, the explicit terminal, then the whole
  accumulated plan optimizes (§5) and executes at that one point, materializing a
  result. Until then nothing ran (target-experience §3: "nothing ran until here";
  `nucleo-expr-spec.md` §5.2).
- **4.6** As a developer, when I force the same plan handle twice, then the result is
  materialized once and re-forcing returns the cached result rather than re-executing
  (`nucleo-expr-spec.md` §5.4).
- **4.7** As a developer, when a relational operation cannot be satisfied (a join key absent
  from a schema, an `agg` over a non-aggregable expression), then it is a compile error
  where the schema makes it knowable, and a fail-loud force-time error otherwise
  (`nucleo-expr-spec.md` §5.5). It is never a silent empty result.

> **Resolved:** [F-FORCE] Transforms are lazy, and forcing is always explicit or self-evident. No
> transform may secretly force. This is the "no magic" rule. The lazy operations are `filter`,
> `select`/`with`, `groupBy`/`agg`, `sort`, `join`, `window`/`rolling`, and `resample`, which
> build plan nodes. The terminals force the minimal necessary plan. They are `collect()`, which
> materializes the table; scalar reductions (`ticks.price.max()`, `.sum()`), which yield a
> concrete value, since asking for one number is self-evidently "compute now", so the
> target-experience `ticks.price.max() - …` needs no `collect`; `fetch(n)`/`head(n)`, a bounded
> preview; and iteration (`for (row : table)`), since you wrote a loop. There is one lazy
> `Table<T>` type and no separate eager mode, so the optimizer always sees the whole plan. An
> eager convenience mode is a deferrable maybe. This maps to the engine's force triggers
> (`nucleo-expr-spec.md` [X5]).

## 5. Query optimization — predicate & projection pushdown

The plan is optimized before execution by the same optimizer the tensor engine uses
(`nucleo-expr-spec.md` §4), over the same graph, because a dataframe pipeline is an
expression graph. The two headline relational rewrites are predicate pushdown, which applies
filters as early as possible and ideally at scan time, and projection pushdown, which reads only
the columns the pipeline consumes. The frame's job is to build the plan and expose the pushed
predicate and projection to the scan. The engine performs the rewrite (§4 of the expr spec).

**Use cases**
- **5.1** As a developer, when I write `scanParquet(...).filter(col.price > 0).groupBy(...)`,
  then the plan is built and optimized before any row is processed, and nothing executes until
  `collect()` (`nucleo-expr-spec.md` §4.1).
- **5.2** As a developer, when a `filter` sits above a scan, then predicate pushdown moves
  the predicate to the scan so failing rows are never read, never materialized, and never carried
  through the pipeline. A more selective predicate does measurably less work
  (`nucleo-expr-spec.md` §4.2).
- **5.3** As a developer, when my pipeline references only a subset of a wide table's columns,
  then projection pushdown restricts the scan to those columns so unreferenced column
  buffers are never read (`nucleo-expr-spec.md` §4.3). The typed schema makes the referenced
  set statically knowable.
- **5.4** As a developer, when a pushed predicate reaches a scan over a source that carries an
  index with statistics (zone-maps / per-chunk min-max, §9), then whole chunks provably
  outside the predicate are skipped entirely. The engine exposes the pushed predicate and the
  index subsystem performs the skip (`nucleo-expr-spec.md` §4.4). See §6.
- **5.5** As a developer, when a `filter → groupBy → sort` chain is optimized, then the
  optimizer may reorder or combine stages where provably equivalent (e.g. filter before groupby)
  without changing the result set (`nucleo-expr-spec.md` §4.5).

## 6. Predicate pushdown via zone-maps (the index payoff)

The concrete payoff of the index interface (§9) for the common analytical scan is the
zone-map, a per-chunk min/max that lets a pushed predicate skip whole Arrow chunks without
reading them. This is the cheapest index, and it serves the dominant case (analysis §4.5).

**Use cases**
- **6.1** As a developer, when I scan a chunked source whose chunks carry zone-maps and apply a
  selective range predicate (`col.price > 100.0`), then chunks whose `[min, max]` lies entirely
  outside the predicate are skipped and their column buffers are never read. The result is
  identical to scanning every chunk, because skipping is sound and never lossy.
- **6.2** As a núcleo author, when the engine pushes a predicate to a scan, then the scan
  consults each chunk's zone-map via the index interface (§9) to decide whether to read the
  chunk. A chunk that might contain matching rows is read and the predicate is applied
  row-wise. Zone-maps prune, and they do not filter.
- **6.3** As a developer, when a source has no zone-map, or the predicate is not
  range-shaped, then the scan reads all chunks and applies the predicate row-wise. The
  absence of an index degrades to a full scan and never to a wrong answer.
- **6.4** As a núcleo author, when a column source carries other statistics the binning
  primitive produces (per-feature quantile sketches), then the same zone-map machinery
  generalizes, one primitive in two roles, without the frame committing to a second structure.
  The frame owns this quantile-sketch / zone-map primitive (here and §9). `nucleo.trees`
  (`trees-spec.md`) is a consumer of it for histogram binning (analysis §3.6/§4.5). The
  primitive is defined once here and is not duplicated there.

## 7. Real nullable types — not NaN-as-missing

Missing data is a real absence carried by the column's validity bitmap
(`nucleo-column-spec.md` §3), surfaced through the schema's nullable field types. It is never a
NaN sentinel, which is the pandas mistake this refuses (analysis §3.5).

**Use cases**
- **7.1** As a developer, when a schema field is nullable (`float64?`), then its column is
  `Column<float64?>` and a missing element is a true absence consulted via the validity bitmap,
  distinct from a present `NaN` value. `NaN` is a number and missing is missing, and the two are
  never conflated.
- **7.2** As a developer, when I aggregate over a nullable column (`ticks.lastTrade.mean()`),
  then nulls are skipped or propagated by a defined, explicit rule. They are never silently
  treated as zero and never NaN-poison the whole reduction. The null-handling default is
  specified rather than accidental (`nucleo-expr-spec.md` §7.3 / [X7]).
- **7.3** As a developer, when a relational operation propagates validity (a filter, a computed
  column over a nullable input), then the engine carries the validity bitmap through the
  expression (`nucleo-expr-spec.md` §7.3). A non-null column runs the raw-buffer fast path with
  no nullability branch.
- **7.4** As a developer, when I need to convert missing to present (fill) or drop missing rows,
  then there is an explicit operation (`fillNull(...)`, `dropNulls(...)`) producing a column
  whose type may narrow to non-null. There is never an implicit coercion.

> **Resolved:** the default null-handling rule per operation family follows SQL/Polars
> semantics, gated by the type. A non-null `Column<T>` runs the no-null fast path, and these
> rules apply only where the schema says `T?`:
> - **Arithmetic / elementwise:** null propagates (any null operand → null).
> - **Comparison in a filter:** three-valued; a `null`-valued predicate excludes the row.
> - **Aggregations** (`sum`/`mean`/`min`/`max`/`std`): skip nulls; `mean = sum / non-null
>   count`; `count()` = non-null, `len()` = all rows.
> - **Sort:** nulls last (overridable). **Group-by:** null is its own group.
>   **Join:** null does not match null.
> - **Fill/drop:** explicit only (`fillNull(v)` may narrow `Column<T?>` → `Column<T>`,
>   `dropNulls()`); never implicit.
>
> Crucially, null ≠ NaN. `mean([1,2,null]) = 1.5` because null is skipped, but
> `mean([1,2,NaN]) = NaN` because NaN is a value that propagates. These are two distinct,
> predictable behaviors, where pandas conflates them. The engine carries validity through
> expressions (`nucleo-expr-spec.md` [X7] / `nucleo-column-spec.md`). Two defaults are flagged as
> adjustable, sort nulls-last and join null-no-match.

## 8. Time-series resample — the one worth porting

Resample regroups a time-indexed table into fixed time buckets and aggregates each bucket. It
is the one genuinely valuable, poorly served pandas feature explicitly worth porting
(analysis §3.5). It is expressed lazily over a time column, with the bucketing key explicit.
There is no implicit DatetimeIndex, because there is no Index.

```cajeta
var minuteBars = ticks.resample(col.ts, every: 1.minutes).agg(col.price.last());
```

**Use cases**
- **8.1** As a developer, when I call `resample(col.ts, every: 1.minutes)`, then rows are
  grouped into fixed-width time buckets keyed by the explicit time column `col.ts` and an
  interval, building a lazy group node. No implicit time Index is required, since the time
  column is named explicitly, unlike pandas' DatetimeIndex.
- **8.2** As a developer, when I aggregate a resample (`.agg(col.price.last())`), then each
  bucket reduces by the given aggregation expressions (`last`, `mean`, `ohlc`-style first/last/
  min/max, sum, count). This is the same column-expression aggregation as `groupBy` (§4.4),
  keyed by time bucket.
- **8.3** As a developer, when the source is unsorted on the time column, then resample's
  contract is defined. It requires or establishes time ordering, or it fails loud, and it never
  silently produces mis-bucketed results. The ordering precondition is explicit.
- **8.4** As a developer, when I resample at an interval coarser or finer than the data's
  native cadence (downsample / upsample), then the behavior is defined. Downsampling aggregates
  many rows per bucket. Upsampling's empty-bucket policy (skip vs. emit a null/forward-filled
  bucket) is an explicit argument and never an implicit default.

> **Resolved:** the resample surface:
> - **Interval type:** a typed `Duration` (`every: 1.minutes`, `5.seconds`, `1.days`) with fixed,
>   well-defined widths, leaning on the typed-duration pattern. Calendar offsets (months/years,
>   which have variable width and need calendar arithmetic) are a later addition and are outside
>   v1.
> - **Bucket-edge alignment:** a deterministic origin. Buckets align to epoch-aligned
>   boundaries by default (a `1.minutes` bucket starts at `:00`), left-closed, with explicit
>   `origin`/`offset`/`closed` overrides. There is no data-dependent or implicit alignment.
> - **Upsample empty-bucket policy:** an empty bucket is null by default, consistent with real
>   nulls, since no data is missing rather than zero. Fill is explicit (`fillNull`/forward-fill).
>   There is never an implicit fill (§8.4).
> - **Rolling/window sharing:** `resample` (fixed non-overlapping time buckets) and `rolling`
>   (sliding overlapping windows) are distinct surfaces that share the bucket-assignment /
>   windowing primitive in the engine. That is one mechanism with two APIs (cf.
>   `nucleo-expr-spec.md`).

## 9. The pluggable index interface (the only commitment)

The index interface is the single retrofit-expensive commitment of the indexing story.
Every concrete implementation is a deferrable backlog behind it (analysis §4.5). A column
advertises what it can be indexed by, a query asks for a capability, and the structure is
swappable. Because Arrow batches are immutable, indexes are bulk-loaded bottom-up and never
dynamically inserted into.

**Use cases**
- **9.1** As a núcleo author, when I define the index contract, then a column may advertise an
  index capability (e.g. "I can answer range predicates", "I can answer a sorted-key point/
  range lookup", "I can answer a low-dimensional spatial box") and a query asks for a capability
  rather than a concrete structure. The structure behind a capability is therefore swappable
  without changing the query.
- **9.2** As a núcleo author, when an index is built over an immutable Arrow batch, then it is
  bulk-loaded in one bottom-up pass (e.g. bottom-up B+, STR-packed for spatial) and never
  incrementally dynamic-inserted. The immutability of the batch is the design premise rather
  than a limitation to work around.
- **9.3** As a developer, when a query carries a predicate matching an available index
  capability, then the optimizer routes it to the index (chunk skip via zone-map §6, key lookup
  via B+ §9.5) and falls back to a full scan when no capability matches. The index only ever
  accelerates a correct answer and never changes it. Absence of an index degrades performance
  and never correctness.

### 9.4 Deferred, correctly placed
- **9.4.1** As a núcleo author, when extended-object box queries become a demonstrated
  workload, then an R-tree capability is added behind the interface. It is not added before
  that, so there is no speculative second tree type.
- **9.4.2** As a graphics author, when I need geometric acceleration, then BVH lives in the
  geometry subsystem over tensor buffers rather than over table rows (analysis §4.5, §4.6).
  Coupling a BVH to the dataframe index is an explicit category error and is refused here.
- **9.4.3** As an ML author, when high-dimensional nearest-neighbor search is needed (embeddings),
  then HNSW / IVF arrive with the embedding/Torch work behind the same interface. They are not
  part of the dataframe v1.

### 9.5 The near-term implementation backlog
- **9.5.1** As a developer, when I index a sorted scalar key column, then an in-memory B+,
  bulk-loaded, answers point and range lookups on that key. It is the second near-term
  structure after zone-maps.
- **9.5.2** As a developer, when I have a low-dimensional spatial table query (e.g. a 2-D
  range), then Z-order-over-B+ answers it by reusing the B+ tree on a Z-ordered key. There is
  no second tree type, which is a deliberate reuse (analysis §4.5).
- **9.5.3** As a developer, when I rely on the cheapest analytical-scan acceleration, then
  zone-maps (per-chunk min/max, §6) are the first structure. They serve the common
  analytical scan and demand no sorted key.

> **Resolved (minimal + provisional, shaped by the first impl):** [F-INDEX] We pin only the
> minimal contract the near-term indexes need, and we let the first implementation (zone-maps)
> finalize the exact shape. Designing a rich contract before any index exists is speculative
> over-design, and that is the trap to avoid. The minimal v1 contract:
> - **Capability vocabulary:** a small set for the three near-term structures, `ChunkSkip`
>   (min/max), `RangePredicate`, and `PointLookup`. Spatial and nearest-neighbor capabilities
>   are not defined until R-tree / HNSW workloads arrive (§9.4).
> - **Request/match:** a request is a predicate shape on a column, and matching is exact
>   (capability-kind + column). Cost-based selection among multiple matching indexes is deferred.
> - **Result type:** an index returns a chunk-mask (which chunks to read) for v1, which is
>   what pushdown (§6) consumes. Richer results (row-set / bitmap / position range) are added when
>   a structure needs them.
> - **Lifecycle:** bulk-load at table creation (§9.2), attach to a column, discovered by the
>   optimizer.
> - **Fallback guarantee:** unchanged and load-bearing. A missing or unmatched capability degrades
>   to a correct full scan and never to a wrong answer (§9.3).
>
> The backlog order stands: zone-maps → in-memory B+ → Z-order-over-B+ (§9.5). R-tree/HNSW go
> behind the same interface only when a workload demands them (§9.4). The contract is
> provisional. The zone-map implementation is expected to refine it, and that is intended rather
> than a failure.

## 10. Arrow interop at the table boundary

A table enters from and exits to the Arrow world zero-copy at its boundary, delegating to
`nucleo-column-spec.md`'s C-Data-Interface seam (`ArrowSchema`/`ArrowArray`, no `libarrow`).
The frame's role is to map a whole table's columns to and from a set of Arrow arrays. The
per-column zero-copy guarantee is the column spec's (§4 there).

**Use cases**
- **10.1** As a developer, when I `Table.scanParquet<Tick>("ticks.parquet")`, then the file's
  columns are scanned into the table's `Column<T>` set zero-copy where the on-disk encoding
  matches the in-memory layout. Decode is delegated to the codec lib, which materializes into
  Arrow-conformant columns (`nucleo-column-spec.md` §10 cross-spec). The result is a typed
  `Table<Tick>` whose schema is checked against the file's schema, and a mismatch is a fail-loud
  error rather than a silent reshape.
- **10.2** As a developer, when I `frame.exportArrow()`, then I receive `ArrowSchema`/`ArrowArray`
  handles over the table's live column buffers, and pyarrow / Polars / DuckDB read the frame
  with no serialization and no copy (target-experience §7; per-column guarantee
  `nucleo-column-spec.md` §4.1).
- **10.3** As a developer, when I import an externally-produced set of Arrow arrays as a
  `Table<T>`, then each column is imported zero-copy honoring the producer's release callback
  (`nucleo-column-spec.md` §4.2/§4.3), and the imported column set is checked against `T`'s
  schema. A null-count of zero admits a non-null column, and a nonzero count admits only the
  nullable field (`nucleo-column-spec.md` §3.5).
- **10.4** As a developer, when a scanned or imported column carries an extension type (MXFP4 etc.),
  then the table admits it as the corresponding logical column and the bytes move even where a
  consumer does not know the semantics (`nucleo-column-spec.md` §5, graceful degradation).

## 11. Acceptance criteria (spec-level)
- A `Table<T>` for a record `T` has exactly one typed column per field, derived at compile time.
  A non-record schema parameter is a compile error.
- Typed column access (`ticks.price`) resolves to the field's typed column. A typo
  (`ticks.prce`) is a compile error rather than a runtime lookup.
- There is no Index/MultiIndex, no `inplace=`, no object-dtype, no NaN-as-missing
  (real `Column<T?>` instead), and no copy/view ambiguity. Each is demonstrably absent.
- A `filter → groupBy → agg → sort` chain builds a lazy plan that optimizes before executing
  and forces only at the explicit terminal (`collect()`).
- Predicate pushdown and projection pushdown are observable. A more selective filter or a
  narrower projection reads measurably fewer rows or columns.
- Zone-map pushdown skips whole chunks for a selective range predicate, producing a result
  identical to a full scan. Absence of a zone-map degrades to a full scan and never to a wrong
  answer.
- A join, a time-series `resample`, and a `groupBy/agg` each produce correct, typed results over
  the lazy engine.
- A table scans from Parquet, exports zero-copy to pyarrow, and imports an external Arrow array
  set, all via `nucleo-column-spec.md`'s seam, with the schema checked at the boundary.
- The index interface is one pluggable contract. A capability is advertised by a column and
  requested by a query, indexes are bulk-loaded, and a missing index never changes a result.

## 12. Open questions (resolve at plan time)
- **[F7] RESOLVED:** both `col.price` (expression builder, late-bound to an op's input) and
  `ticks.price` (member access bound to a specific table) over the same typed columns, distinct
  roles, one synthesizer; dynamic `col("...")` on a `Table<?>` (§3, §4.3). Joint with
  `records-spec.md` [F7].
- **[F-FORCE] RESOLVED:** transforms lazy; force is explicit-or-self-evident (no transform
  secretly forces); terminals = `collect()` / scalar reductions / `fetch(n)`·`head(n)` /
  iteration; one lazy `Table<T>` type, no separate eager mode (§4).
- **[F-INDEX] RESOLVED (minimal + provisional):** pin only the minimal contract the near-term
  indexes need (`ChunkSkip`/`RangePredicate`/`PointLookup` capabilities; exact match; chunk-mask
  result; bulk-load lifecycle; fallback-to-scan), and let the first impl (zone-maps) finalize the
  shape (§9). Backlog: zone-maps → B+ → Z-order-over-B+.
- **[F-NAME] RESOLVED:** primary type `Table<T>` in `dev.cajeta.nucleo.frame`; `DataFrame` is
  the pandas-façade alias (§1.4).
- **Derived-schema typing RESOLVED (provisional):** gradual via existing wildcards. A
  schema-changing op yields a visible `Table<?>`, narrowed by a checked `.as<R>()` or accessed
  dynamically via `col("...")`; covariant reads via `Table<? extends T>`. No anonymous records,
  no magic, diagnosable errors; fully-typed-pipeline (computed schemas) deferred (§4.3).
  The variance dependency is CONFIRMED by code audit. Use-site wildcards plus capture-projection
  map exactly onto this model (§4.3).
- **Null-handling rule RESOLVED:** SQL/Polars semantics, type-gated (propagate in arithmetic,
  skip in aggregations, three-valued filter, nulls-last sort, null-own-group, null-no-match join,
  explicit fill/drop); null ≠ NaN (§7). Engine carries validity (`nucleo-expr-spec.md` [X7]).
- The resample surface details, which are the interval type, bucket-edge alignment, upsample
  empty-bucket policy, and `rolling`/window machinery sharing (§8).
- **v1 milestone scope RESOLVED:** v1 = scan, `filter`, `select`/`with`, `groupBy`/`agg`,
  `sort`, `join` (too core to defer), and `resample` (the headline). `window`/`rolling` and
  reshape (`pivot`/`melt`) are milestone 2, since they are more complex and less load-bearing
  for first use. Resolves with `nucleo-expr-spec.md` scope.
- **Table column set vs. schema RESOLVED:** exactly the record's fields; "at least `T`'s
  columns" reads via `Table<? extends T>` wildcards (§2, §4.3.3).
