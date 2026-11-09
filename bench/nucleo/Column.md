# Column

`cajeta.nucleo.column` holds the Arrow-laid-out columnar buffers under a table.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `nucleo/column-filter` | filter a 1e8 int64 column by a mask (the mask as the validity bitmap of a zero-copy `NullableColumn`, then `dropNulls()`; there is no filter kernel yet) | Apache Arrow compute (pyarrow), polars Series, NumPy | count |
| `nucleo/column-arith` | `a + b * c` over 1e8 float64 columns with nulls (through `asTensor`, `Tensor.mul` and `add`, validity ANDed; `Column` has no arithmetic yet) | same | checksum |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: nucleo/column-filter
metric: throughput
```

```bench
id: nucleo/column-arith
metric: throughput
```

## Standing

Filtering a column is 103x behind polars and 30x to 50x behind pyarrow and numpy; arithmetic 42x behind numpy. The column ops run on the cpu tensor paths, where the three libraries run vectorized kernels. Follows the tensor-element-access work (benchmark-reference 22.2.8).

## Reproduce

```sh
cd bench
./bench.sh --bench column-filter               # Cajeta
./bench.sh --bench column-filter --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Column](../../docs/stdlib/nucleo/Column.md)
- Source: `runtime/src/cajeta/nucleo/column/`
- Benchmark source: `bench/src/main/cajeta/bench/nucleo/`
