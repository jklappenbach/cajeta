# ImmutableMap

`cajeta.collection.ImmutableMap` — the frozen hash-indexed map snapshot.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/immutablemap-build` | freeze a 1e6-entry map, 10 times | Java `Map.copyOf`, Rust `HashMap` behind `Arc`, Go map copy, Python `MappingProxyType` | count |
| `collection/immutablemap-lookup` | 1e7 lookups | same | sum |

On v0.46.3 each trial of `immutablemap-build` is slower than the one before it. A smoke run measured 51 s, 144 s and 222 s for its three trials, and resident memory rose from 1.4 GB to 3.5 GB across them (2026-10-25). Inside a whole `--area collection` smoke run it went past an hour (the rest of the package takes under three minutes). That growth is a known gap, not the cost of a freeze.

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
