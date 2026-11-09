# cajeta.nucleo

This section measures the dataframe and the autodiff and fusion intrinsics, against pandas, polars and DuckDB, and against PyTorch and JAX for the transforms.

| Page | |
|---|---|
| [Table](Table.md) | The lazy, typed dataframe whose operations are built then run |
| [Column](Column.md) | The Arrow-laid-out columnar buffers under a table |
| [Tape](Tape.md) | The define-by-run autograd tape |
| [Fuse](Fuse.md) | Compile-time fusion of elementwise tensor expressions: the fused row against the same expression written as separate operations |
| [Transforms](Transforms.md) | The compile-time function transforms Grad, Vmap and Jit |

## Standing

Geometric mean 142x behind over 29 benchmarks, no wins. vmap is within 1.7x of jax; everything else is 15x to 909x behind. The columns, tables and grad run on the cpu tensor paths against polars, duckdb, numpy and torch's parallel vectorized engines. The tensor-element-access work comes first, the engine after.
