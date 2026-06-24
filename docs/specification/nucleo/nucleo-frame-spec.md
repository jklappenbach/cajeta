# Núcleo Frame — Specification

> Status: draft for review (2026-06-23). The **Polars-shaped lazy typed dataframe** —
> núcleo's tabular surface (`dev.cajeta.nucleo.frame`), including the **pluggable index
> interface**. Layer-1b. Companion analysis: `python-stack-analysis.md` §3.5 (pandas →
> Polars) / §4.5 (indexing); siblings `nucleo-column-spec.md` (the columnar buffer it
> stores over), `nucleo-expr-spec.md` (the lazy expression engine it plans on),
> `records-spec.md` (the record that *is* the schema), `source-synthesis-spec.md` (the
> facility that synthesizes typed column accessors). Design context: `target-experience.md`
> §3.
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §12, to be
> resolved when this spec is turned into a plan. Outline-numbered for addressability.

## 1. Definition

### 1.1 Purpose
A **Table** is a typed, lazy, expression-planned dataframe: a set of named columns whose
**schema lives in the type** (`Table<Tick>`, where `Tick` is a record), over which
relational operations (`filter`, `select`, `groupBy`, `agg`, `sort`, `join`, window/rolling,
pivot/melt, time-series resample) **build a plan that a query optimizer rewrites before it
executes**. It exists to give Cajeta the *tabular abstraction* — heterogeneous named columns
with relational operations — that is genuinely fundamental, **without** porting pandas'
specific DataFrame, which is not (analysis §3.5). The reference shape is **Polars** (lazy, no
index, expression-based, query-optimized) and **DuckDB** (logical plan → optimizer →
vectorized engine), over **Arrow** bytes (`nucleo-column-spec.md`).

The distinctive move only a typed language can make: **the schema is in the type**. Column
names and types are known at compile time, so every `KeyError` and silent dtype coercion
becomes a compile error and `ticks.price` autocompletes (analysis §3.5; Haskell's *Frames*
proves the idea, nobody has done it in a language aimed at scientific users).

### 1.2 Scope
- A **typed dataframe** `Table<T>` whose column set is derived at compile time from the
  record `T`'s fields (`nucleo-column-spec.md` SoA: one `Column` per field).
- A **lazy, expression-based relational API** — `filter`, `select`/`with`, `groupBy`/`agg`,
  `sort`, `join`, window/`rolling`, `pivot`/`melt`, and an explicit terminal `collect()` —
  that builds a plan on the expression engine (`nucleo-expr-spec.md`) and forces only at the
  terminal.
- **Query optimization** at the frame level: **predicate pushdown** and **projection
  pushdown** to the scan (the engine performs the rewrites; the frame exposes the plan).
- **Real nullable types** (`Column<T?>`) as the missing-data model — not NaN-as-missing.
- **Typed column access** (`ticks.price`) via source-synthesized accessors
  (`source-synthesis-spec.md`); a typo is a **compile error**.
- **Time-series resample** — the one genuinely-worth-porting pandas feature.
- The **pluggable index interface** (§9): one contract a column advertises and a query asks
  for; every concrete index is a deferrable backlog behind it. Indexes are **bulk-loaded**
  (Arrow batches are immutable).
- **Arrow C-Data interop at the import/export boundary** — `scanParquet` in, `exportArrow`
  out — delegated to `nucleo-column-spec.md`.

### 1.3 Non-goals
- **The columnar buffer itself** — physical layout, validity bitmaps, Arrow interchange live
  in `nucleo-column-spec.md`. This spec *stores over* columns; it does not define them.
- **The expression IR, the optimizer passes, fusion, lowering** — `nucleo-expr-spec.md`. This
  spec defines the *relational surface* those passes plan and run; it does not define the
  engine.
- **The pandas mistakes, dropped on purpose** (analysis §3.5): **Index / MultiIndex** and
  their implicit alignment; **`inplace=`**; the **object-dtype** zoo; **NaN-as-missing**;
  the **copy/view ambiguity** and `SettingWithCopyWarning` (Polars eliminated mutation
  entirely). None of these are ports — they are explicit refusals.
- **Concrete index implementations.** The *interface* (§9) is the only commitment; zone-maps,
  in-memory B+, and Z-order-over-B+ are a near-term backlog, R-tree / BVH / HNSW deferred and
  correctly placed (§9.4) — not specified here as built components.
- **A cost-based query optimizer / statistics-driven join reordering.** v1 planning is the
  rule-based rewrites the engine commits to (predicate + projection pushdown); cost-based
  planning is deferred (`nucleo-expr-spec.md` [X4]).
- **Cross-machine / distributed execution.** Single-process, the engine's CPU-or-GPU target.
- **An eager pandas-style `DataFrame`.** `Table` is lazy-first; the optional
  `dev.cajeta.pandas` recognizability skin (analysis §3.5) is a separate façade, not this
  spec.

### 1.4 Relationship to existing constructs
- **Records are the schema.** `Table<Tick>` derives its columns from `Tick`'s fields by
  compile-time reflection (`records-spec.md` §6): column `price` has type `float64`, etc. A
  record is the *type-level* descriptor; the table's physical store is one `Column<T>` per
  field (`records-spec.md` §7, SoA — **not** an array of record instances).
- **Columns are the storage.** Each field lands on a `Column<T>` (non-null) or `Column<T?>`
  (nullable) per `nucleo-column-spec.md`; a non-null numeric column is bit-identical to a
  tensor buffer, so the same engine computes over table columns and tensors.
- **The expression engine is the planner.** `col.price > 0.0`, `(col.price * col.size).sum()`,
  and the `filter → groupBy → agg → sort` chain all build nodes in the *same* lazy graph the
  tensor engine uses (`nucleo-expr-spec.md` §2/§4); the frame is a relational *skin* over that
  one engine (analysis §2.2).
- **Source synthesis generates the accessors.** Typed member access `ticks.price` is produced
  by the Tier-A synthesizer keyed on the `Table<T>` instantiation
  (`source-synthesis-spec.md` §1.2: generic-instantiation trigger), re-checked like
  hand-written code.
- **Templates are monomorphized** — `Table<Tick>` and `Table<Splat>` are distinct, fully
  typed types with distinct synthesized accessors and distinct column sets; no erasure.
- **Named arguments + defaults** (✅ today) carry the keyword API surface
  (`resample(col.ts, every: 1.minutes)`, `join(other, on: ..., how: Left)`).

> **Resolved:** [F-NAME] The primary type is **`Table<T>`** in module **`dev.cajeta.nucleo.frame`**.
> It synergizes with the record decision (a **record** is a *typed row*; a **`Table<T>`** is rows
> sharing the schema `T` — the database lineage records already lean on), collides with nothing,
> and carries no pandas baggage. **`DataFrame`** is reserved as the **pandas-façade alias**
> (`dev.cajeta.pandas`) — migrants get the familiar name, mapped onto `Table` underneath. (The
> module is named for the conceptual area; the `Table` type living under `…frame` is intentional.)

## 2. The typed table — schema in the type

A `Table<T>` is parameterized by a record `T` whose fields *are* the schema. The column set,
each column's name, and each column's type are fixed at compile time.

```cajeta
record Tick { Instant ts; float64 price; float64 size; Symbol venue; }
Table<Tick> ticks = Table.scanParquet<Tick>("ticks.parquet");
```

**Use cases**
- **2.1** As a developer, when I write `Table<Tick>` for a record `Tick`, then the table has
  exactly one column per `Tick` field, each column's name and type derived from the field at
  compile time (`records-spec.md` §6.1) — no runtime schema discovery.
- **2.2** As a developer, when I declare a `Table<T>` where `T` is **not** a record, then it is
  a compile error — the schema parameter must be a record (the type-level descriptor).
- **2.3** As a developer, when a record field is a nullable type (`float64?`), then the
  corresponding column is `Column<float64?>` (carries a validity bitmap), and a non-null field
  (`float64`) is `Column<float64>` (no bitmap) — nullability is a per-column type fact, not a
  runtime dtype flag (`nucleo-column-spec.md` §3).
- **2.4** As a developer, when I have two tables of the **same** schema (`Table<Tick>`), then
  they are the same type and combine without re-alignment; two tables of **different** schemas
  are different types and only the operations that bridge schemas (join, select-into-new-record)
  connect them — there is **no implicit Index alignment** (the pandas sin, dropped).
- **2.5** As a developer, when I print or inspect an unforced `Table<T>`, then I see its schema
  and its (unexecuted) plan, not its rows — building never executes (`nucleo-expr-spec.md`
  §2.5/§5.1).

> **Resolved:** A `Table<T>` has **exactly** `T`'s fields as its columns (the record *is* the
> table's type). The "operate on any table that has *at least* `T`'s columns" case is expressed
> by the bounded wildcard `Table<? extends T>` (§4.3.3) — via subtyping, not by a table carrying
> columns beyond its schema. The schema stays exact; wildcards handle the superset reads.

## 3. Typed column access — the typo is a compile error

A table exposes its columns two ways, both compile-time-checked against the schema: a typed
**member accessor** (`ticks.price`) and a **column-expression builder** (`col.price`). Either
way, a name not in the schema fails to compile — never a runtime `KeyError`.

**Use cases**
- **3.1** As a developer, when I read a typed member accessor (`ticks.price`), then it resolves
  to the `float64` column for that field; a typo (`ticks.prce`) is a **compile error**, not a
  runtime lookup (analysis §3.5; target-experience §3). *(The accessor is generated by the
  source-synthesis facility keyed on the `Table<T>` instantiation —
  `source-synthesis-spec.md`.)*
- **3.2** As a developer, when I build a column expression in a relational operation
  (`filter(col.price > 0.0)`), then `col.price` builds a typed column-reference node in the
  expression graph (`nucleo-expr-spec.md` §2.2); a name not in the schema is a compile error,
  and the comparison's operand type is the field's type (a `col.venue > 0.0` type mismatch is a
  compile error too).
- **3.3** As a developer, when I read a whole-column scalar through the member accessor
  (`ticks.price.max()`), then it is a typed column operation whose result type follows from the
  field type, and a typo anywhere in the chain is a compile error
  (`var spread = ticks.price.max() - ticks.price.min();` — target-experience §3).
- **3.4** As a developer, when I reference a column that the schema declares nullable
  (`ticks.lastTrade` of `float64?`), then its accessor is typed nullable and operations on it
  carry null-aware semantics (§7); a non-null column's accessor carries no nullability branch
  (`nucleo-column-spec.md` §3.3).

> **Resolved:** [F7] **Both**, with distinct, non-redundant roles over the same typed columns:
> - **`col.price`** — the typed column-**expression builder**, late-bound to a relational op's
>   *input* schema, used *inside* transforms (`filter(col.price > 0.0)`, `agg(col.price.sum())`).
> - **`ticks.price`** — typed member access **bound to a specific table**, used for direct/terminal
>   access on that table (a scalar reduction `ticks.price.max()`, extracting a column).
>
> Both are schema-checked (a typo is a compile error on a typed `Table<T>`) and both are generated
> by the one source-synthesis synthesizer keyed on `Table<T>`. On a `Table<?>` neither typed form
> is available — you use the dynamic `col("price")` (§4.3.2). Resolved jointly with
> `records-spec.md` [F7].

## 4. The lazy relational API

Relational operations are **built, not run**: each composes a node onto the plan, and nothing
executes until an explicit terminal forces it. This is the Polars/DuckDB model (analysis §3.5)
and rides the engine's lazy graph directly (`nucleo-expr-spec.md` §2).

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
  new lazy `Table`/plan node — no rows are read or materialized at the call
  (`nucleo-expr-spec.md` §5.1) and the original table is unchanged (no `inplace=`, no
  copy/view ambiguity — the pandas sins, dropped).
- **4.2** As a developer, when I chain operations
  (`.filter(...).groupBy(...).agg(...).sort(...)`), then the chain accumulates into a single
  plan whose root is the final operation, with earlier operations as inputs
  (`nucleo-expr-spec.md` §2.3).
- **4.3** As a developer, when I `select`/`with`/`groupBy`+`agg`/`join` to a **differently-shaped
  result**, then the precise nominal schema is no longer statically known, so the result is a
  **schema-erased `Table<?>`** — the erasure is *visible in the type* (the `?`), not hidden. The
  relational plan still tracks the real columns (for force-time validation), but the static
  surface is `Table<?>` until re-bound. Three ways to proceed, all ordinary language features:
  - **4.3.1 (narrow)** As a developer, when I `.as<VenueVwap>()` on a `Table<?>`, then it narrows
    to `Table<VenueVwap>` — a **checked** cast: the real (plan/runtime) schema is validated
    against `VenueVwap`'s fields/types and a mismatch is a clear located error (*"column `vwap`
    absent / has type X; `VenueVwap.vwap` is `float64`"*), never a silent wrong type.
  - **4.3.2 (dynamic)** As a developer, when I access a column on a `Table<?>` without narrowing,
    then I use the dynamic accessor `col("vwap")` (string-keyed, force-time validated); the typed
    `df.vwap` member is *not* available on `Table<?>`, and the error says exactly that: *"schema
    not statically known here; narrow with `.as<R>()` or use `col(\"...\")`."*
  - **4.3.3 (covariant read)** As a developer, when I write a function over *any table with at
    least `Tick`'s columns*, then I type the parameter `Table<? extends Tick>` (the existing
    bounded wildcard) — it accepts `Table<Tick>` and `Table<TradeTick>`, with `Tick`'s typed
    accessors available because the bound guarantees them.

> **Resolved (provisional):** derived-schema typing is **gradual, via existing wildcards** —
> *typed at the edges, dynamic in the middle*. A named source `Table<Tick>` and an explicitly
> re-bound `Table<R>` (`.as<R>()`) are statically typed; a schema-changing op yields a visible
> `Table<?>` you narrow or access dynamically. **No anonymous structural records** (records
> decision holds), **no magic** (just generics + `? extends` + a checked cast — the language's
> normal rules, no bespoke type theory), **diagnosable errors** (ordinary wildcard/cast messages
> that name the fix). The fully-typed-pipeline option (the compiler *computing* a nominal result
> schema) is **deferred** — it is precisely the path that introduces type-inference "magic" and
> hard-to-diagnose errors we are avoiding; it stays open only if it can be done without them.
> **Confirmed (2026-06-23, code audit):** cajeta supports use-site wildcard variance — the grammar
> accepts `<?>`/`<? extends T>`/`<? super T>` (`CajetaParser.g4`), and `CajetaClass::isAssignableToWildcard`
> makes `Table<TradeTick>` a genuine subtype of `Table<? extends Tick>` (tested, `TemplateWildcardP6Tests`).
> It integrates with monomorphization via **capture-projection** (`CajetaType::captureProject`):
> `<? extends Tick>` projects to `Tick` for member access (so §4.3.3's accessors resolve), while an
> unbounded `<?>` is pointer-only (so §4.3.2 must narrow or use `col("...")`) — cajeta's existing
> wildcard semantics map *exactly* onto this model. (`.as<R>()` is a frame-level checked narrowing
> that validates the runtime schema — frame logic, not a wildcard-machinery dependency.)
- **4.4** As a developer, when I call `groupBy(keys).agg(exprs)`, then the keys and the
  aggregation expressions build a group-aggregate node; the aggregation expressions are
  column expressions over the engine (`(col.price * col.size).sum()` is an elementwise fusion
  feeding a reduction — `nucleo-expr-spec.md` §7.2), not opaque string operations.
- **4.5** As a developer, when I call `.collect()` (the explicit terminal), then the whole
  accumulated plan **optimizes** (§5) and **executes** at that one point, materializing a
  result; until then nothing ran (target-experience §3: "nothing ran until here";
  `nucleo-expr-spec.md` §5.2).
- **4.6** As a developer, when I force the same plan handle twice, then the result is
  materialized once and re-forcing returns the cached result rather than re-executing
  (`nucleo-expr-spec.md` §5.4).
- **4.7** As a developer, when a relational operation cannot be satisfied (a join key absent
  from a schema, an `agg` over a non-aggregable expression), then it is a **compile error**
  where the schema makes it knowable, and a fail-loud force-time error otherwise
  (`nucleo-expr-spec.md` §5.5) — never a silent empty result.

> **Resolved:** [F-FORCE] **Transforms are lazy; forcing is always explicit or self-evident — no
> transform may secretly force** (the "no magic" rule). Lazy: `filter`, `select`/`with`,
> `groupBy`/`agg`, `sort`, `join`, `window`/`rolling`, `resample` (build plan nodes). Terminals
> (force the minimal necessary plan): **`collect()`** (materialize the table), **scalar
> reductions** (`ticks.price.max()`, `.sum()` → a concrete value — asking for one number is
> self-evidently "compute now", so the target-experience `ticks.price.max() - …` needs no
> `collect`), **`fetch(n)`/`head(n)`** (bounded preview), and **iteration** (`for (row : table)` —
> you wrote a loop). **One lazy `Table<T>` type** — no separate eager mode (the optimizer always
> sees the whole plan; an eager convenience mode is a deferrable maybe). Maps to the engine's
> force triggers (`nucleo-expr-spec.md` [X5]).

## 5. Query optimization — predicate & projection pushdown

The plan is optimized **before** execution by the same optimizer the tensor engine uses
(`nucleo-expr-spec.md` §4), over the same graph — because a dataframe pipeline *is* an
expression graph. The two headline relational rewrites are **predicate pushdown** (apply
filters as early as possible, ideally at scan time) and **projection pushdown** (read only the
columns the pipeline consumes). The frame's job is to **build the plan and expose the pushed
predicate/projection to the scan**; the engine performs the rewrite (§4 of the expr spec).

**Use cases**
- **5.1** As a developer, when I write `scanParquet(...).filter(col.price > 0).groupBy(...)`,
  then the plan is built and optimized before any row is processed, and nothing executes until
  `collect()` (`nucleo-expr-spec.md` §4.1).
- **5.2** As a developer, when a `filter` sits above a scan, then **predicate pushdown** moves
  the predicate to the scan so failing rows are never read, never materialized, never carried
  through the pipeline — a more selective predicate does measurably less work
  (`nucleo-expr-spec.md` §4.2).
- **5.3** As a developer, when my pipeline references only a subset of a wide table's columns,
  then **projection pushdown** restricts the scan to those columns so unreferenced column
  buffers are never read (`nucleo-expr-spec.md` §4.3) — the typed schema makes the referenced
  set statically knowable.
- **5.4** As a developer, when a pushed predicate reaches a scan over a source that carries an
  index with statistics (zone-maps / per-chunk min-max — §9), then whole chunks provably
  outside the predicate are skipped entirely; the engine exposes the pushed predicate and the
  index subsystem performs the skip (`nucleo-expr-spec.md` §4.4) — see §6.
- **5.5** As a developer, when a `filter → groupBy → sort` chain is optimized, then the
  optimizer may reorder/combine stages where provably equivalent (e.g. filter before groupby)
  without changing the result set (`nucleo-expr-spec.md` §4.5).

## 6. Predicate pushdown via zone-maps (the index payoff)

The concrete payoff of the index interface (§9) for the common analytical scan: a **zone-map**
(per-chunk min/max) lets a pushed predicate skip whole Arrow chunks without reading them. This
is the cheapest index and serves the dominant case (analysis §4.5).

**Use cases**
- **6.1** As a developer, when I scan a chunked source whose chunks carry zone-maps and apply a
  selective range predicate (`col.price > 100.0`), then chunks whose `[min, max]` lies entirely
  outside the predicate are skipped — their column buffers are never read — and the result is
  identical to scanning every chunk (skipping is sound, never lossy).
- **6.2** As a núcleo author, when the engine pushes a predicate to a scan, then the scan
  consults each chunk's zone-map via the index interface (§9) to decide whether to read the
  chunk; a chunk that *might* contain matching rows is read and the predicate is applied
  row-wise (zone-maps prune, they do not filter).
- **6.3** As a developer, when a source has **no** zone-map (or the predicate is not
  range-shaped), then the scan reads all chunks and applies the predicate row-wise — the
  absence of an index degrades to a full scan, never to a wrong answer.
- **6.4** As a núcleo author, when a column source carries other statistics the binning
  primitive produces (per-feature quantile sketches), then the same zone-map machinery
  generalizes (one primitive, two hats), without the frame committing to a second structure.
  **The frame owns this quantile-sketch / zone-map primitive** (here and §9); `nucleo.trees`
  (`trees-spec.md`) is a *consumer* of it for histogram binning (analysis §3.6/§4.5) — the
  primitive is defined once here, not duplicated there.

## 7. Real nullable types — not NaN-as-missing

Missing data is a **real absence** carried by the column's validity bitmap
(`nucleo-column-spec.md` §3), surfaced through the schema's nullable field types — **not** a
NaN sentinel (the pandas mistake, refused: analysis §3.5).

**Use cases**
- **7.1** As a developer, when a schema field is nullable (`float64?`), then its column is
  `Column<float64?>` and a missing element is a true absence consulted via the validity bitmap,
  distinct from a present `NaN` value (`NaN` is a number; missing is missing — they are not
  conflated).
- **7.2** As a developer, when I aggregate over a nullable column (`ticks.lastTrade.mean()`),
  then nulls are skipped/propagated by a **defined, explicit** rule (not silently treated as
  zero or as NaN-poisoning the whole reduction) — the null-handling default is specified, not
  accidental (`nucleo-expr-spec.md` §7.3 / [X7]).
- **7.3** As a developer, when a relational operation propagates validity (a filter, a computed
  column over a nullable input), then the engine carries the validity bitmap through the
  expression (`nucleo-expr-spec.md` §7.3); a non-null column runs the raw-buffer fast path with
  no nullability branch.
- **7.4** As a developer, when I need to convert missing to present (fill) or drop missing rows,
  then there is an **explicit** operation (`fillNull(...)`, `dropNulls(...)`) producing a column
  whose type may narrow to non-null — never an implicit coercion.

> **Resolved:** the default null-handling rule per operation family follows **SQL/Polars
> semantics**, gated by the type (a non-null `Column<T>` runs the no-null fast path; these rules
> apply only where the schema says `T?`):
> - **Arithmetic / elementwise:** null **propagates** (any null operand → null).
> - **Comparison in a filter:** three-valued — `null`-valued predicate → row **excluded**.
> - **Aggregations** (`sum`/`mean`/`min`/`max`/`std`): **skip nulls**; `mean = sum / non-null
>   count`; `count()` = non-null, `len()` = all rows.
> - **Sort:** nulls **last** (overridable). **Group-by:** null is its **own group**.
>   **Join:** null does **not** match null.
> - **Fill/drop:** explicit only (`fillNull(v)` may narrow `Column<T?>` → `Column<T>`,
>   `dropNulls()`); never implicit.
>
> Crucially, **null ≠ NaN**: `mean([1,2,null]) = 1.5` (null skipped) but `mean([1,2,NaN]) = NaN`
> (NaN is a value that propagates) — two distinct, predictable behaviors, vs. pandas' conflation.
> The engine carries validity through expressions (`nucleo-expr-spec.md` [X7] /
> `nucleo-column-spec.md`). Adjustable defaults flagged: sort nulls-last, and join null-no-match.

## 8. Time-series resample — the one worth porting

**Resample** — regrouping a time-indexed table into fixed time buckets and aggregating — is the
one genuinely-valuable, poorly-served-elsewhere pandas feature explicitly worth porting
(analysis §3.5). It is expressed lazily over a time column, with the bucketing key explicit
(no implicit DatetimeIndex — there is no Index).

```cajeta
var minuteBars = ticks.resample(col.ts, every: 1.minutes).agg(col.price.last());
```

**Use cases**
- **8.1** As a developer, when I call `resample(col.ts, every: 1.minutes)`, then rows are
  grouped into fixed-width time buckets keyed by the explicit time column `col.ts` and an
  interval, building a lazy group node — **no implicit time Index** is required (the time
  column is named explicitly, unlike pandas' DatetimeIndex).
- **8.2** As a developer, when I aggregate a resample (`.agg(col.price.last())`), then each
  bucket reduces by the given aggregation expressions (`last`, `mean`, `ohlc`-style first/last/
  min/max, sum, count) — the same column-expression aggregation as `groupBy` (§4.4), keyed by
  time bucket.
- **8.3** As a developer, when the source is unsorted on the time column, then resample's
  contract is defined (it requires/establishes time ordering, or fails loud) rather than
  silently producing mis-bucketed results — the ordering precondition is explicit.
- **8.4** As a developer, when I resample at an interval coarser or finer than the data's
  native cadence (downsample / upsample), then the behavior is defined: downsampling aggregates
  many rows per bucket; upsampling's empty-bucket policy (skip vs. emit a null/forward-filled
  bucket) is an explicit argument, not an implicit default.

> **Resolved:** the resample surface:
> - **Interval type:** a typed **`Duration`** (`every: 1.minutes`, `5.seconds`, `1.days`) — fixed,
>   well-defined widths (leans on the typed-duration pattern). **Calendar offsets** (months/years —
>   variable width, needing calendar arithmetic) are a *later* addition, not v1.
> - **Bucket-edge alignment:** a deterministic origin — buckets align to **epoch-aligned
>   boundaries** by default (a `1.minutes` bucket starts at `:00`), **left-closed**, with explicit
>   `origin`/`offset`/`closed` overrides. No data-dependent or implicit alignment.
> - **Upsample empty-bucket policy:** an empty bucket is **null** by default (consistent with real
>   nulls — no data is *missing*, not zero), with explicit fill (`fillNull`/forward-fill); never an
>   implicit fill (§8.4).
> - **Rolling/window sharing:** `resample` (fixed non-overlapping time buckets) and `rolling`
>   (sliding overlapping windows) are **distinct surfaces** that **share the bucket-assignment /
>   windowing primitive** in the engine — one mechanism, two APIs (cf. `nucleo-expr-spec.md`).

## 9. The pluggable index interface (the only commitment)

The **index interface** is the single retrofit-expensive commitment of the indexing story;
every concrete implementation is a deferrable backlog behind it (analysis §4.5). A column
**advertises** what it can be indexed by; a query **asks** for a capability; the structure is
**swappable**. Because Arrow batches are immutable, indexes are **bulk-loaded** (bottom-up,
never dynamic-insert).

**Use cases**
- **9.1** As a núcleo author, when I define the index contract, then a column may advertise an
  index *capability* (e.g. "I can answer range predicates", "I can answer a sorted-key point/
  range lookup", "I can answer a low-dimensional spatial box") and a query asks for a capability
  rather than a concrete structure — so the structure behind a capability is swappable without
  changing the query.
- **9.2** As a núcleo author, when an index is built over an (immutable) Arrow batch, then it is
  **bulk-loaded** in one bottom-up pass (e.g. bottom-up B+, STR-packed for spatial), never
  incrementally dynamic-inserted — the immutability of the batch is the design premise, not a
  limitation to work around.
- **9.3** As a developer, when a query carries a predicate matching an available index
  capability, then the optimizer routes it to the index (chunk skip via zone-map §6, key lookup
  via B+ §9.5) and falls back to a full scan when no capability matches — the index **only ever
  accelerates a correct answer**, never changes it (soundness: absence of an index degrades
  performance, not correctness).

### 9.4 Deferred, correctly placed
- **9.4.1** As a núcleo author, when extended-object **box queries** become a demonstrated
  workload, then an **R-tree** capability is added behind the interface — but not before
  (no speculative second tree type).
- **9.4.2** As a graphics author, when I need geometric acceleration, then **BVH** lives in the
  **geometry subsystem over tensor buffers, not table rows** (analysis §4.5, §4.6) — coupling a
  BVH to the dataframe index is an explicit category error and is refused here.
- **9.4.3** As an ML author, when high-dimensional nearest-neighbor search is needed (embeddings),
  then **HNSW / IVF** arrive with the embedding/Torch work behind the same interface — not part
  of the dataframe v1.

### 9.5 The near-term implementation backlog
- **9.5.1** As a developer, when I index a sorted scalar key column, then an **in-memory B+**
  (bulk-loaded) answers point and range lookups on that key — the second near-term structure
  after zone-maps.
- **9.5.2** As a developer, when I have a low-dimensional spatial *table* query (e.g. a 2-D
  range), then **Z-order-over-B+** answers it by reusing the B+ tree on a Z-ordered key — **no
  second tree type** (a deliberate reuse, analysis §4.5).
- **9.5.3** As a developer, when I rely on the cheapest analytical-scan acceleration, then
  **zone-maps** (per-chunk min/max, §6) are the first structure — they serve the common
  analytical scan and demand no sorted key.

> **Resolved (minimal + provisional — shaped by the first impl):** [F-INDEX] We pin only the
> **minimal contract the near-term indexes need**, and let the **first implementation (zone-maps)
> finalize the exact shape** — designing a rich contract before any index exists is speculative
> over-design (the trap to avoid). The minimal v1 contract:
> - **Capability vocabulary:** a small set for the three near-term structures — `ChunkSkip`
>   (min/max), `RangePredicate`, `PointLookup`. (Spatial / nearest-neighbor capabilities are
>   *not* defined until R-tree / HNSW workloads arrive — §9.4.)
> - **Request/match:** a request is a *predicate shape on a column*; matching is **exact**
>   (capability-kind + column). Cost-based selection among multiple matching indexes is deferred.
> - **Result type:** an index returns a **chunk-mask** (which chunks to read) for v1 — that is
>   what pushdown (§6) consumes. Richer results (row-set / bitmap / position range) are added when
>   a structure needs them.
> - **Lifecycle:** bulk-load at table creation (§9.2), attach to a column, discovered by the
>   optimizer.
> - **Fallback guarantee:** unchanged and load-bearing — a missing/unmatched capability degrades
>   to a correct full scan, never a wrong answer (§9.3).
>
> Backlog order stands: **zone-maps → in-memory B+ → Z-order-over-B+** (§9.5); R-tree/HNSW behind
> the same interface only when a workload demands (§9.4). The contract is **provisional**: the
> zone-map implementation is expected to refine it, and that's intended, not a failure.

## 10. Arrow interop at the table boundary

A table enters from and exits to the Arrow world **zero-copy** at its boundary, delegating to
`nucleo-column-spec.md`'s C-Data-Interface seam (`ArrowSchema`/`ArrowArray`, no `libarrow`).
The frame's role is to map a whole table's columns to/from a set of Arrow arrays; the per-column
zero-copy guarantee is the column spec's (§4 there).

**Use cases**
- **10.1** As a developer, when I `Table.scanParquet<Tick>("ticks.parquet")`, then the file's
  columns are scanned into the table's `Column<T>` set zero-copy where the on-disk encoding
  matches the in-memory layout (delegating decode to the codec lib materializing into Arrow-
  conformant columns — `nucleo-column-spec.md` §10 cross-spec), and the result is a typed
  `Table<Tick>` whose schema is checked against the file's schema (a mismatch is a fail-loud
  error, not a silent reshape).
- **10.2** As a developer, when I `frame.exportArrow()`, then I receive `ArrowSchema`/`ArrowArray`
  handles over the table's live column buffers, and pyarrow / Polars / DuckDB read the frame
  **with no serialization and no copy** (target-experience §7; per-column guarantee
  `nucleo-column-spec.md` §4.1).
- **10.3** As a developer, when I import an externally-produced set of Arrow arrays as a
  `Table<T>`, then each column is imported zero-copy honoring the producer's release callback
  (`nucleo-column-spec.md` §4.2/§4.3), and the imported column set is checked against `T`'s
  schema (null-count zero admits a non-null column; nonzero admits only the nullable field —
  `nucleo-column-spec.md` §3.5).
- **10.4** As a developer, when a scanned/imported column carries an extension type (MXFP4 etc.),
  then the table admits it as the corresponding logical column and the bytes move even where a
  consumer does not know the semantics (`nucleo-column-spec.md` §5 — graceful degradation).

## 11. Acceptance criteria (spec-level)
- A `Table<T>` for a record `T` has exactly one typed column per field, derived at compile time;
  a non-record schema parameter is a compile error.
- Typed column access (`ticks.price`) resolves to the field's typed column; a typo
  (`ticks.prce`) is a **compile error**, not a runtime lookup.
- There is **no Index/MultiIndex**, **no `inplace=`**, **no object-dtype**, **no NaN-as-missing**
  (real `Column<T?>` instead), and **no copy/view ambiguity** — each is demonstrably absent.
- A `filter → groupBy → agg → sort` chain builds a lazy plan that optimizes **before** executing
  and forces only at the explicit terminal (`collect()`).
- **Predicate pushdown** and **projection pushdown** are observable: a more selective filter / a
  narrower projection reads measurably fewer rows / columns.
- Zone-map pushdown skips whole chunks for a selective range predicate, producing a result
  identical to a full scan; absence of a zone-map degrades to a full scan, never a wrong answer.
- A join, a time-series `resample`, and a `groupBy/agg` each produce correct, typed results over
  the lazy engine.
- A table scans from Parquet, exports zero-copy to pyarrow, and imports an external Arrow array
  set — all via `nucleo-column-spec.md`'s seam, with the schema checked at the boundary.
- The **index interface** is one pluggable contract: a capability is advertised by a column and
  requested by a query; indexes are bulk-loaded; a missing index never changes a result.

## 12. Open questions (resolve at plan time)
- **[F7] — RESOLVED:** both `col.price` (expression builder, late-bound to an op's input) and
  `ticks.price` (member access bound to a specific table) over the same typed columns, distinct
  roles, one synthesizer; dynamic `col("...")` on a `Table<?>` (§3, §4.3). Joint with
  `records-spec.md` [F7].
- **[F-FORCE] — RESOLVED:** transforms lazy; force is explicit-or-self-evident (no transform
  secretly forces); terminals = `collect()` / scalar reductions / `fetch(n)`·`head(n)` /
  iteration; one lazy `Table<T>` type, no separate eager mode (§4).
- **[F-INDEX] — RESOLVED (minimal + provisional):** pin only the minimal contract the near-term
  indexes need (`ChunkSkip`/`RangePredicate`/`PointLookup` capabilities; exact match; chunk-mask
  result; bulk-load lifecycle; fallback-to-scan), and let the first impl (zone-maps) finalize the
  shape (§9). Backlog: zone-maps → B+ → Z-order-over-B+.
- **[F-NAME] — RESOLVED:** primary type `Table<T>` in `dev.cajeta.nucleo.frame`; `DataFrame` is
  the pandas-façade alias (§1.4).
- **Derived-schema typing — RESOLVED (provisional):** gradual via existing wildcards — a
  schema-changing op yields a visible `Table<?>`, narrowed by a checked `.as<R>()` or accessed
  dynamically via `col("...")`; covariant reads via `Table<? extends T>`. No anonymous records,
  no magic, diagnosable errors; fully-typed-pipeline (computed schemas) deferred (§4.3).
  **Variance dependency CONFIRMED** (code audit — use-site wildcards + capture-projection map
  exactly onto this model; §4.3).
- **Null-handling rule — RESOLVED:** SQL/Polars semantics, type-gated (propagate in arithmetic,
  skip in aggregations, three-valued filter, nulls-last sort, null-own-group, null-no-match join,
  explicit fill/drop); null ≠ NaN (§7). Engine carries validity (`nucleo-expr-spec.md` [X7]).
- The resample surface details — interval type, bucket-edge alignment, upsample empty-bucket
  policy, and `rolling`/window machinery sharing (§8).
- **v1 milestone scope — RESOLVED:** v1 = scan, `filter`, `select`/`with`, `groupBy`/`agg`,
  `sort`, **`join`** (too core to defer), and `resample` (the headline). **`window`/`rolling` and
  reshape (`pivot`/`melt`) are milestone 2** (more complex, less load-bearing for first use).
  Resolves with `nucleo-expr-spec.md` scope.
- **Table column set vs. schema — RESOLVED:** exactly the record's fields; "at least `T`'s
  columns" reads via `Table<? extends T>` wildcards (§2, §4.3.3).
