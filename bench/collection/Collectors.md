# Collectors

`cajeta.collection.Collectors` — the standard `Collector` factories for stream terminal operations.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/collectors-to-list` | `stream().collect(toList())` over 1e7 int32 | Java `Collectors.toList`, Rust `collect::<Vec>`, Go append loop, Python list comprehension | count |
| `collection/collectors-group-by` | group 1e7 int32 by `x % 1000` through a hand-built `Collector` (`Collectors` ships `toList` only) | Java `groupingBy`, Rust `HashMap` fold, Go map of slices, Python `defaultdict` | group count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/collectors-to-list
metric: time
```

```bench
id: collection/collectors-group-by
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench collectors-to-list               # Cajeta
./bench.sh --bench collectors-to-list --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Collectors](../../docs/stdlib/collection/Collectors.md)
- Source: `runtime/src/cajeta/collection/Collectors.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
