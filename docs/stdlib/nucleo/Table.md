# Table — the lazy, typed dataframe

`cajeta.nucleo.frame` — a columnar dataframe whose schema is a **record** and
whose operations are **built, not run**. `Table<T>` has exactly one typed column
per field of `T`, derived at compile time; a chain of relational operations
accumulates a lazy plan that optimizes *before* it executes and forces only at an
explicit terminal. It sits on the `cajeta.nucleo.column` substrate (Arrow-laid-out
buffers) and speaks the same frozen Arrow C ABI at its boundaries — no `libarrow`.

The design is Polars/DuckDB, not pandas: there is **no Index/MultiIndex**, **no
`inplace=`**, **no object-dtype**, **no NaN-as-missing** (real `Column<T?>`
instead), and **no copy/view ambiguity** — each demonstrably absent
(`SpecAcceptanceTests`).

## The record IS the schema

```cajeta
public record Tick { Instant ts; float64 price; float64 size; Utf8 venue; }

Table<Tick> t = heap Table<Tick>(
    Column.of<int64>(tsv), Column.of<float64>(pv),
    Column.of<float64>(sv), StringColumn.of(vv));

t.price.get(0);      // typed column access — the field's typed column
t.prce;              // COMPILE error, not a runtime lookup — names no member
```

One typed column per field, in field order. A non-record schema argument
(`Table<SomeClass>`) is a named compile error (`CAJETA_ERROR_FRAME_SCHEMA`), not a
silent degenerate table. `@Nullable` fields become nullable columns; a missing
element is a **true absence** on the validity bitmap, never a NaN sentinel.

## The lazy relational API

Each op composes a node onto the plan and returns a new lazy handle; nothing reads
or materializes until a terminal. The spec's headline, run verbatim
(`VwapEndToEndTests`):

```cajeta
Table<VenueVwap> r = t.lazy()
    .filter((TickCols c) -> { Pred p = c.price() > 0.0; return #p; })
    .groupBy((TickCols c, Sels s) -> { s.add(c.venue()); })
    .agg((TickCols c, Aggs ag) -> {
        ag.add(((c.price() * c.size()).sum() / c.size().sum()).alias("vwap"));
    })
    .as<VenueVwap>()
    .sort((VenueVwapCols c, Sorts s) -> { s.add(c.vwap().desc()); })
    .collect();                       // nothing ran until here
```

- **Two column roles, one synthesizer.** `c.price()` in a lambda is the
  expression builder (late-bound to the op's input); `t.price` is member access
  bound to a specific table. Same typed columns, distinct roles.
- **Terminals** (force): `collect()`, scalar reductions, `fetch(n)`/`head(n)`,
  iteration. No transform secretly forces — laziness is observable (`h.scanRows()`
  reads `-1` on an unforced handle, the row count after).
- **The input is never mutated.** A relational op yields a new table; re-deriving
  from the same source again gives the full set.

v1 ops: `scan`, `filter`, `select`/`with`, `groupBy`/`agg`, `sort`, `join`,
`resample`. Milestone 2 (shipped in the complete-scope build): `rolling`/`window`,
`pivot`/`melt`.

## Gradual typing — schema-erased `Table<?>`, narrowed by the type system

A schema-changing op (`select`, `groupBy`+`agg`, `join`) produces a
**differently-shaped result**, so its nominal schema is no longer statically
known: the result is a **visible `Table<?>`** — the erasure is in the type, not
hidden. The plan still tracks the real columns for force-time validation. Three
ways forward, all ordinary language features:

- **narrow** — `.as<VenueVwap>()` is a *checked* cast: the real schema is
  validated against the record's fields/types, a mismatch is a clear located
  error (*"column `vwap` absent / has type X; `VenueVwap.vwap` is `float64`"*),
  never a silent wrong type.
- **dynamic** — `col("vwap")` reads a column on an erased table, string-keyed and
  force-time validated. The typed `df.vwap` member is *not* available on
  `Table<?>`, and the error says exactly that.
- **covariant read** — a function over *any table with at least `T`'s columns*
  types its parameter `Table<? extends T>` (the existing bounded wildcard), with
  `T`'s typed accessors available because the bound guarantees them.

## Null handling — SQL/Polars semantics, null ≠ NaN

Missing data is a real absence carried by the validity bitmap, type-gated:
**propagate** in arithmetic, **skip** in aggregations, three-valued filter,
nulls-last sort, null-own-group, null-no-match join, explicit fill/drop.
Crucially `null ≠ NaN`: `mean([1,2,null]) = 1.5` (null skipped) but
`mean([1,2,NaN]) = NaN` (NaN is a value that propagates) — two distinct,
predictable behaviors, not the pandas conflation.

## Pushdown + the index interface

Optimization **accelerates a correct answer, never changes it**:

- **Predicate & projection pushdown** are observable — a more selective filter
  reads measurably fewer rows (`scanRows()`), a narrower projection fewer columns
  (`scanCols()`). Filter sinks below a `groupBy` when it reads only key columns.
- **The index interface is one pluggable contract**: a column *advertises* a
  capability (`ChunkSkip`/`RangePredicate`/`PointLookup`), a query *requests* one;
  match is exact, indexes are bulk-loaded, and a **missing index never changes a
  result** — it degrades to a full scan. Shipped indexes: zone-maps (per-chunk
  min/max, skips provably-outside chunks), an in-memory B+ (point/range → position
  set), and Z-order-over-B+ (Morton-interleaved 2-D box).

## Boundaries — Arrow in, Arrow out, Parquet via cajeta-codec

A table exports zero-copy to the Arrow C Data Interface (one
`{ArrowSchema, ArrowArray}` bundle per column, over live buffers) and imports an
external Arrow array set checked against the record:

```cajeta
int64[] bundles = t.exportArrow();                     // zero-copy borrow
Table<Bar> r = Table.importArrow<Bar>(schemas, arrays, n);   // checked rebind
```

Parquet decode lives in **cajeta-codec** (`dev.cajeta.codec.parquet`, full
fidelity: BYTE_ARRAY, definition-levels → validity, multi-row-group, typed
wrappers); its `ParquetArrow` helper produces Arrow-seam handles that
`importArrow<R>` consumes. `scanParquet` as a stdlib method would be a layering
violation — the stdlib exposes the seam; the ecosystem library reaches over it.

## Deferred (spec-sanctioned)

Out of v1 scope by explicit spec resolution, behind stable interfaces:

- **Calendar `resample` offsets** (month/quarter/year boundaries) — the interval
  grid is nanos-based; calendar alignment is deferred.
- **R-tree / HNSW** spatial & vector indexes — behind the same index contract as
  zone-maps/B+/Z-order; addable without touching query code.
- **Cost-based optimization** — the current rewriter is rule-based
  (pushdown/pruning); a cost model is deferred.
- **A separate eager mode** — there is one lazy `Table<T>`; no eager `DataFrame`
  variant (the `DataFrame` name is only the pandas-façade alias).
- **`dev.cajeta.pandas` façade** — the pandas-shaped surface over `Table` is its
  own spec.
- **Engine-level fusion unification** — sharing nucleo-expr's fusion machinery is
  deferred to that spec.
- **Surface sugar** — `as "x"` naming, `1.minutes` durations, for-each iteration
  sugar live in the syntax-sugar spec; here they read as `.alias("x")` and
  explicit nanos.

## CSV — blocked

`Table.fromCsv<R>` is blocked on a toolchain class-loader gap (first
instantiation of a member-synthesized `Table<R>` inside a template-static body
does not materialize the synthesized column class). The concrete parser is
designed and ready to resurrect once the loader force-loads member-synthesis
imports at instantiation. The Arrow/Parquet boundaries are the shipped ingress.
