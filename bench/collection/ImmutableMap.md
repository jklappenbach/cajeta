# ImmutableMap

`cajeta.collection.ImmutableMap` — the frozen hash-indexed map snapshot.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/immutablemap-build` | freeze a 1e6-entry map, 10 times (through `entries().forEach` into fresh pairs; building from a collected list throws because the pairs are borrows) | Java `Map.copyOf`, Rust `HashMap` behind `Arc`, Go map copy, Python `MappingProxyType` | count |
| `collection/immutablemap-lookup` | 1e7 lookups | same | sum |

`immutablemap-build` is a known gap on v0.46.3 and is recorded as a skip: each build is slower than the last (51, 144, 222 s over three trials) with resident memory rising from 1.4 to 3.5 GB, filed as [immutablemap-build-leak](../../specs/immutablemap-build-leak-spec.md). The smoke prints "gap closed" the day the row runs clean.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/immutablemap-build
metric: time
```

```bench
id: collection/immutablemap-lookup
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench immutablemap-build               # Cajeta
./bench.sh --bench immutablemap-build --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [ImmutableMap](../../docs/stdlib/collection/ImmutableMap.md)
- Source: `runtime/src/cajeta/collection/ImmutableMap.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
