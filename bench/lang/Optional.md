# Optional

`cajeta.lang.Optional` — the value-typed sum, present or empty. A by-value `Optional<int32>` should cost what a tagged pair of words costs; the row says whether it does.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/optional-map-unwrap` | 1e8 `Optional<int32>` constructed, tested with `isPresent`, rebuilt and unwrapped, half empty (`Optional` has no `map`) | C++ `std::optional`, Rust `Option`, Go (value, ok), Java `OptionalInt`, Python `None` | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/optional-map-unwrap
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench optional-map-unwrap               # Cajeta
./bench.sh --bench optional-map-unwrap --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Optional](../../docs/stdlib/lang/Optional.md)
- Source: `runtime/src/cajeta/lang/Optional.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
