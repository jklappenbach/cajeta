# Distance

`cajeta.search.distance.Distance` — string edit distances.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `search/distance-levenshtein` | 1e6 pairs of 16-byte strings | Rust `strsim`, Python `rapidfuzz`, Java commons-text, Go `agnivade/levenshtein` | sum |
| `search/distance-jaro-winkler` | 1e6 pairs | same | sum within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: search/distance-levenshtein
metric: throughput
```

```bench
id: search/distance-jaro-winkler
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench distance-levenshtein               # Cajeta
./bench.sh --bench distance-levenshtein --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Distance](../../../docs/stdlib/search/distance/Distance.md)
- Source: `runtime/src/cajeta/search/distance/Distance.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/search/`
