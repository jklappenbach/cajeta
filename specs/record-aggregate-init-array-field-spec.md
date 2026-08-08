# record-aggregate-init-array-field — a record aggregate initializer rejects an array-typed field

## 1. Definition

Found 2026-08-08 implementing nucleo-distributed-frame U1.1.3 (nested
column types). A record that DECLARES an array-typed field parses and
compiles, but the aggregate initializer that constructs it always fails:

```cajeta
public record R { float64[] a; float64 b; }

float64[] tmp = Q.mk();          // or Q.mk() inline — same result
R r = R { a: tmp, b: 3.0 };
// CAJETA_ERROR_AGGREGATE_INIT_TYPE: aggregate initializer for 'R':
//   value bound to field 'a' does not match the field's type
```

Bounding probes:
- No frame, no generics, no ownership marker is involved — an 11-line
  standalone program reproduces it.
- Both spellings fail identically: a method result bound inline
  (`a: Q.mk()`) and a plain local bound by name (`a: tmp`), so this is
  not the owned-temp (`#T[]`) rule.
- The same record with only scalar fields constructs normally, so the
  initializer itself works — it is the ARRAY-typed field it will not
  bind.

Net effect: a record with an array field is declarable but not
constructible, so such a record has no value form at all.

## 2. Requirements

- **2.1** A record with an array-typed field constructs through its
  aggregate initializer, binding both an owned array result and a
  local.
- **2.2** A regression pin constructs a record with an array field and
  reads an element back.

## 3. Impact / workaround (in use)

`Table<T>` derives a LIST column from a `T[]` record field
(nucleo-distributed-frame §10.5.3). The column surface is entirely
unaffected — the schema derives, the list is queryable, and
`ListColumn.rowF64(i)` (and its per-physical siblings) hands a row back
as an array. Only the TYPED ROW form is blocked, because `rowAt`
rebuilds the record through exactly this initializer. `Table.rowAt`
therefore emits a directed failure for a list column, naming the
column-wise accessor, rather than half-building a row. Struct columns
(record-typed fields) reconstruct normally — their children are
scalars.

## 4. Reproduction

The program in §1 as a standalone `--emit=exe` run under the
`fix/nucleo-frame-seams` toolchain; also
`NestedColumnTests::nestedColumnsSurviveAPlan`, whose last assertion
PINS the directed `rowAt` failure — that assertion is the one to
invert when this is fixed.
