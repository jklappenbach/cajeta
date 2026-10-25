# ImmutableSet

`cajeta.collection.ImmutableSet` — the frozen hash-indexed set.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/immutableset-build` | freeze a 1e6 set, 10 times | Java `Set.copyOf`, Rust, Go, Python frozenset | count |
| `collection/immutableset-contains` | 1e7 `contains` | same | hit count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/immutableset-build
metric: time
```

```bench
id: collection/immutableset-contains
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench immutableset-build               # Cajeta
./bench.sh --bench immutableset-build --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [ImmutableSet](../../docs/stdlib/collection/ImmutableSet.md)
- Source: `runtime/src/cajeta/collection/ImmutableSet.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
