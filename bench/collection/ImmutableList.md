# ImmutableList

`cajeta.collection.ImmutableList` — the immutable array-backed list.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/immutablelist-build` | build from a 1e6 `ArrayList`, 1e2 times | Java `List.copyOf`, Rust `Arc<[T]>`, Go slice copy, Python tuple | count |
| `collection/immutablelist-get` | 1e8 random gets | same | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/immutablelist-build
metric: time
```

```bench
id: collection/immutablelist-get
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench immutablelist-build               # Cajeta
./bench.sh --bench immutablelist-build --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [ImmutableList](../../docs/stdlib/collection/ImmutableList.md)
- Source: `runtime/src/cajeta/collection/ImmutableList.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
