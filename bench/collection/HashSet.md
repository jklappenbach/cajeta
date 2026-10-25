# HashSet

`cajeta.collection.HashSet` — the hash-based set of unique values.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/hashset-dedup` | insert 1e7 int32 drawn from 1e6 distinct values | C++ `unordered_set`, Rust `HashSet`, Go map[int]struct{}, Java `HashSet`, Python set | distinct count |
| `collection/hashset-contains` | 1e7 `contains` on a 1e6 set, half present | same | hit count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/hashset-dedup
metric: time
```

```bench
id: collection/hashset-contains
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench hashset-dedup               # Cajeta
./bench.sh --bench hashset-dedup --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [HashSet](../../docs/stdlib/collection/HashSet.md)
- Source: `runtime/src/cajeta/collection/HashSet.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
