# ImmutableList

`cajeta.collection.ImmutableList` is the immutable array-backed list.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/immutablelist-build` | build from a 1e6 `ArrayList`, 1e2 times (two copies per build: there is no copy-from-a-borrowed-list factory) | Java `List.copyOf`, Rust `Arc<[T]>`, Go slice copy, Python tuple | count |
| `collection/immutablelist-get` | 1e8 random gets | same | sum |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/immutablelist-build
metric: time
```

```bench
id: collection/immutablelist-get
metric: time
```

## Standing

Building is 3.2x behind Rust's Arc slice and get is 2.7x behind a C++ shared_ptr to a const vector; Go's slices.Clone is 2x faster than Cajeta on both. Java's List.copyOf and Python's tuple are slower. Not yet investigated.

## Reproduce

```sh
cd bench
./bench.sh --bench immutablelist-build               # Cajeta
./bench.sh --bench immutablelist-build --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [ImmutableList](../../docs/stdlib/collection/ImmutableList.md)
- Source: `runtime/src/cajeta/collection/ImmutableList.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
