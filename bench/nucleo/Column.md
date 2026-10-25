# Column

`cajeta.nucleo.column` — the Arrow-laid-out columnar buffers under a table.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `nucleo/column-filter` | filter a 1e8 int64 column by a mask | Apache Arrow compute (pyarrow), polars Series, NumPy | count |
| `nucleo/column-arith` | `a + b * c` over 1e8 float64 columns with nulls | same | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: nucleo/column-filter
metric: throughput
```

```bench
id: nucleo/column-arith
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench column-filter               # Cajeta
./bench.sh --bench column-filter --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Column](../../docs/stdlib/nucleo/Column.md)
- Source: `runtime/src/cajeta/nucleo/column/`
- Benchmark source: `bench/src/main/cajeta/bench/nucleo/`
