# Nucleo: columns, tables, autograd and fusion

`cajeta.nucleo` is the data-science layer: Arrow-laid-out columns, a lazy
typed dataframe, a define-by-run autograd tape, and the compile-time
transforms that fuse, differentiate, vectorize and compile tensor
expressions. It sits on [Math](../math/README.md) and serves
[ML](../ml/README.md). The LLM engine does not pass through it; the two
share the substrate beside each other.

## What it is

- **`Column`** (`cajeta.nucleo.column`): typed, contiguous, Arrow-conformant
  buffers with a validity bitmap, the physical substrate a dataframe
  column, a tensor backing and an interchange buffer all share; a column
  aliases a `Tensor` with no copy.
- **`Table<T>`** (`cajeta.nucleo.frame`): a columnar dataframe whose schema
  is a record type, and whose operations are built, then run: `groupBy`,
  `agg`, `join`, `sort`, `filter` compose into a plan that `collect()`
  forces, so a pipeline over a 1e7-row table touches memory once.
- **`Tape`** (`cajeta.nucleo.autograd`): the runtime eager tape; ops compute
  their forward value as they execute and record a node, and `backward`
  replays the recording. Today the tape is scalar, one float32 per node.
- **`Fuse`, `Grad`, `Vmap`, `Jit`** (`cajeta.nucleo.expr`,
  `cajeta.nucleo.transform`): compiler intrinsics, not methods. Applied to a
  lambda they rewrite it: `Fuse` collapses an elementwise chain into one
  kernel, `Grad` differentiates, `Vmap` batches, `Jit` compiles.

## How it uses what is below, and serves what is above

A column's arithmetic goes through `Tensor` kernels; a fused expression is
one `@Kernel` the compiler wrote, launched through the same substrate as any
hand-written one. The ML library's `Frames.design<T>(frame, target)` turns a
`Table<T>` into a named design matrix, and its neural half's autodiff
(`ml.grad`) is its own tape over `Tensor<float32>`, built the same way.

## Where it stands

The [benchmarks](../../../bench/nucleo/README.md) run the h2oai db-benchmark
groupby and join questions against pandas, polars and DuckDB, the column
kernels against Arrow compute and NumPy, and the transforms against JAX and
PyTorch. Their pages record what is missing today: a column filter kernel
and column arithmetic (both go through `Tensor`), `Fuse` over more than one
tensor parameter, `Vmap` over a body that reduces a tensor, `Jit` over a
tensor lambda, a nullable utf8 column for left joins, and a tape past 32
nodes until its growth path is fixed. The `Table` runs single-threaded
against engines that thread by default, and the rows say so.

## Reference

[Column](../../stdlib/nucleo/Column.md), [Table](../../stdlib/nucleo/Table.md),
[Tape](../../stdlib/nucleo/Tape.md), [Fuse](../../stdlib/nucleo/Fuse.md),
[Transforms](../../stdlib/nucleo/Transforms.md).
