# Pair

`cajeta.lang.Pair` — the two-field template value type.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/pair-construct` | 1e8 `Pair<int32,int64>` constructed and both fields summed | C++ `std::pair`, Rust tuple, Go struct, Java record, Python tuple | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/pair-construct
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench pair-construct               # Cajeta
./bench.sh --bench pair-construct --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Pair](../../docs/stdlib/lang/Pair.md)
- Source: `runtime/src/cajeta/lang/Pair.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
