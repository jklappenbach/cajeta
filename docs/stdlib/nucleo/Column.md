# Columns — the Arrow-laid-out substrate

`cajeta.nucleo.column` — typed, contiguous, Arrow-conformant columnar
buffers: the physical substrate a dataframe column, a tensor backing, and an
interop buffer all share. The load-bearing invariant: **a non-null numeric
`Column<T>` is bit-identical to a tensor buffer** — same bytes, no
marshalling. Interop is by **matching the frozen Arrow C ABI** (two structs)
— no `libarrow` anywhere.

The package is lazy: programs that never touch columns don't parse it (or
`cajeta.math`, which it pulls).

## Column\<T> — non-null, tensor-bit-identical

```cajeta
import cajeta.nucleo.column.Column;

float32[] fa = { 1.5f, 2.5f, 3.5f };
Column<float32> c #= Column.of<float32>(fa);
Tensor<float32> t #= c.asTensor();     // ZERO-COPY view — shared bytes
Column<float32> back #= Column.fromTensor<float32>(t);   // zero-copy inverse
```

Owned buffers start 64-byte aligned (an aligned start offset inside an
over-allocated backing). No validity bitmap exists on this type and `get`
carries no validity branch. v1 dtypes: `int8..int64`, `uint8..uint64`,
`float16..float64`; `boolean` (Arrow bit-packs bools) and 128-bit types are
named refusals (`ColumnTypeException`).

## NullableColumn\<T> — the separable validity bitmap

Nullability is a **type distinction** (`Column<T?>` in the spec's notation):

```cajeta
boolean[] ok = { true, false, true };
NullableColumn<float32> n #= NullableColumn.of<float32>(vals, ok);
n.isValid(1);              // false — a real absence, never NaN-as-missing
Column<float32> d1 #= n.fillNulls(0.0f);   // dense, nulls replaced
Column<float32> d2 #= n.dropNulls();       // dense, order kept
```

There is deliberately **no `asTensor()`** here — the dense tensor substrate
is reached only through the explicit materializations above.

## StringColumn — variable-length utf8 (format "u")

Offsets (`int32`, length+1) over one contiguous utf8 data buffer:

```cajeta
String[] vs = { "hola", "x", "columnas" };
StringColumn s #= StringColumn.of(vs);
String v #= s.get(2);       // fresh owned copy of the element's bytes
```

## The C Data Interface — zero-copy interchange

`exportArrow()` (on all column types and on contiguous tensors) returns the
address of a bundle whose head is `{ ArrowSchema, ArrowArray }` over the
column's **live** buffers — a zero-copy borrow. The consumer's `release`
frees the struct shells only, exactly once; the column must outlive the
consumer's reads (round-trip promptly, the `TensorProtocol` discipline).

Import wraps a producer's structs zero-copy as a **foreign-backed** column:

```cajeta
Column<?> w #= Column.importArrow(schemaAddr, arrayAddr);
if (w instanceof Column<float32>) {
    Column<float32> c = (Column<float32>) w;   // reified capture
    Column<float32> mine #= c.materialize();    // the explicit compute crossing
}
```

Foreign columns are read-only borrows: `set`/`asTensor` are named errors
directing to `materialize()`. The producer's releases fire exactly once,
when the imported column drops. `null_count > 0` admits only through
`NullableColumn.importArrow` — the type reflects the physical reality.
`Column.importAsTensor(...)` is the one-step import-and-materialize for
tensor consumers (it lives column-side; `cajeta.math` cannot import this
package).

## MX extension types — cajeta.mxfp4

`MxColumn` carries MX micro-scaling formats as Arrow **extension types**: a
logical name (`cajeta.mxfp4`, block size in `ARROW:extension:metadata`) over
physical packed `uint8` bytes — a view, never a re-encode. A consumer that
knows the extension reconstructs the logical type; one that doesn't still
moves the bytes. Scales buffers and MX kernels ride the autograd/xpu lanes.

## Deferred (recorded)

Storage native mode (zero-copy tensor over foreign memory), boolean columns,
64-bit offsets ("U"), nested layouts, null-carrying utf8 import, in-place
nullable→non-null narrowing, the live pyarrow/Polars/DuckDB probe (needs the
embedding seam; the C-ABI conformance is consumer-tested in-tree), codec
readers materializing into columns, and the device-side story.
