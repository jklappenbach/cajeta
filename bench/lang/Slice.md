# Slice

`cajeta.lang.Slice` — the zero-copy array window behind `arr[a:b]`. Creating one must not copy; iterating one must cost what iterating the array costs.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/slice-window` | 1e7 slices of 64 elements taken from a 1e6 array | C++ `std::span`, Rust `&a[a..b]`, Go `a[a:b]`, Java `Arrays.copyOfRange` (copies; noted), Python `memoryview` | sum of first elements |
| `lang/slice-iterate` | iterate a 1e7-element slice 10 times | same | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/slice-window
metric: time
```

```bench
id: lang/slice-iterate
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench slice-window               # Cajeta
./bench.sh --bench slice-window --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Slice](../../docs/stdlib/lang/Slice.md)
- Source: `runtime/src/cajeta/lang/Slice.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
