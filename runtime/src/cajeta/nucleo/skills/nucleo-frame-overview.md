---
id: nucleo-frame-overview
applies-to: [cajeta.nucleo.frame, cajeta/nucleo/frame/Table]
title: cajeta.nucleo.frame — Table<T>, the record-schema'd lazy dataframe
description: Routing for the typed dataframe — the record IS the schema, column physicals, construction (fromColumns / fromCsv / importArrow), typed accessors + lambda DSL, laziness/collect, the dynamic introspection surface, and the record-placement rules per toolchain version.
---

# cajeta.nucleo.frame — Table&lt;T&gt;

A typed, lazy, columnar dataframe: **a `record` is the schema** — each
field becomes a column with a physical type, a synthesized typed accessor
FIELD (`ticks.price.get(i)` — a typo is a compile error), and a
synthesized constructor. Plans (filter/select/groupBy/…) are lazy;
`collect()` forces.

## Schema record → column physicals

| Record field type | Column physical | Accessor type |
|---|---|---|
| numeric primitive (`int8..int64`, `uint*`, `float32/64`) | `Column<prim>` | same |
| `cajeta.time.Instant` | `Column<int64>` (epoch-nanos) | int64 reads |
| `cajeta.lang.Utf8` | `StringColumn` (utf8) | String reads |
| `@Nullable` numeric primitive | `NullableColumn<prim>` | validity-aware |
| `@Nullable Utf8` | **unsupported** (loud error) | — |
| `String`, class types | **rejected** — records take value types | — |

Field named `rows` collides with Table's member — renamed loudly.

## Task → entry point

| You want to… | Use |
|---|---|
| Build from columns you have | `heap Table<R>(Column.of<...>(...), …)` in SCHEMA ORDER — ctor consumes the columns (`#` formals), lengths must agree |
| Parse CSV text (header-mapped) | `Table.fromCsv<R>(csvText)` — v1 physicals float64/int64/Instant/Utf8, non-nullable; missing column or row-arity mismatch throws `FrameException` |
| Adopt external Arrow arrays zero-copy | `Table.importArrow<R>(schemas, arrays, n)` / export via `table.exportArrow()` |
| Read a cell, typed | the synthesized accessor: `t.price.get(i)` |
| Filter/select/group/sort/join/resample/rolling/pivot/melt | the lambda DSL (`t.filter((c) -> c.price().gt(...))` …) — LAZY handles; schema-changing ops yield `Table<?>` |
| Force execution | `.collect()` (idempotent; `head(n)`/`fetch(n)` force + window) |
| Generic column introspection (no R in scope) | `colCount()`, `colNameAt(i)`, `colTypeAt(i)`, `colNullableAt(i)` + dynamic reads `f64At(name, row)`, `i64At`, `strAt`, `validAt` |
| Retype an erased `Table<?>` | `.as<R>()` — strict schema check (count, names, physicals, nullability) |
| Null handling | `fillNull(name, v)` / `dropNulls([name])` — explicit, never implicit coercion |
| Indexing knobs | `indexColumn(name)` (B+), `indexSpatial(x, y)` (Z-order), `zoneChunkRows(n)` |

## Ownership & lifecycle

- The `fromColumns` ctor **consumes** its columns (`#`): pass fresh
  `Column.of<...>(...)` builds or surrender locals with `#`.
- A handle you chain from is **spent** — re-chain from the source table
  (reusing it throws `FrameException` with exactly that message).
- The lambda DSL's builder objects (`<R>Cols`) are synthesized companions;
  you never construct them.

## Record placement (toolchain-version rule)

- **≥ v0.13.0**: declare records anywhere — cross-file records and
  first-instantiation-inside-template-statics are fully materialized on
  demand.
- **v0.12.x**: cross-file records work for DIRECT `Table<R>` use, but a
  consumer file that precedes its record in directory order could hit the
  (since-fixed) nested-materialize hazard — co-locate the record with its
  heaviest user if you must target v0.12.x.

## Not here (dead-end avoidance)

- No implicit type coercion anywhere — a `float64` column never silently
  reads as int, nulls never silently become values.
- No CSV WRITING from Table (CsvWriter in `cajeta.codec.csv` is
  row-oriented; export via Arrow for columnar interchange).
- No nullable CSV physical yet — `@Nullable` schema + `fromCsv` throws;
  import via Arrow instead.
- ML fitting is NOT here: bridge to `dev.cajeta.ml` with
  `Frames.design<R>(t, target)`.
