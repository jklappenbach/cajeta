# Stats

`cajeta.math.stats.Stats` — descriptive statistics.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/stats-moments` | mean, variance, skew and kurtosis of 1e8 float64 | NumPy and SciPy, Rust `statrs`, `gonum/stat`, Apache Commons Math | within epsilon |
| `math/stats-histogram` | 1e8 values into 1e3 bins | NumPy `histogram`, same libraries | bin counts |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/stats-moments
metric: throughput
```

```bench
id: math/stats-histogram
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench stats-moments               # Cajeta
./bench.sh --bench stats-moments --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Stats](../../../docs/stdlib/math/stats/Stats.md)
- Source: `runtime/src/cajeta/math/stats/Stats.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
