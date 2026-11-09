# Distance

`cajeta.search.distance.Distance` — string edit distances.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `search/distance-levenshtein` | 1e6 pairs of 16-byte strings | Rust `strsim`, Python `rapidfuzz`, Java commons-text, Go `agnivade/levenshtein` | sum |
| `search/distance-jaro-winkler` | 1e6 pairs, the strsim/rapidfuzz definition (`Distance` has none yet; commons-text uses a different definition and is checked against its own sum) | same | sum within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: search/distance-levenshtein
metric: throughput
```

```bench
id: search/distance-jaro-winkler
metric: throughput
```

## Standing

Levenshtein is 8.5x and Jaro-Winkler 4.9x behind rapidfuzz, and strsim is 3x to 6x faster than Cajeta; commons-text and the Go implementations are 1.5x to 3x faster. The distance loops index strings byte by byte, where [String](../../lang/String.md)'s byte scan is 171x behind; not yet investigated.

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
