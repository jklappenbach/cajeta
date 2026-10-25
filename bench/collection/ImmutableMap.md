# ImmutableMap

`cajeta.collection.ImmutableMap` — the frozen hash-indexed map snapshot.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/immutablemap-build` | freeze a 1e6-entry map, 10 times | Java `Map.copyOf`, Rust `HashMap` behind `Arc`, Go map copy, Python `MappingProxyType` | count |
| `collection/immutablemap-lookup` | 1e7 lookups | same | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

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
