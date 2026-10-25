# Stream

`cajeta.lang.stream.Stream` — the pull-protocol base for stdlib iteration. The first two rows were the widest losses of the first suite (an `Optional<T>` per element through virtual dispatch); they are kept unchanged so the chart shows what has moved since.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/stream-filter-map-reduce` | filter → map → reduce over 1e7 int32 | C++ ranges, Rust iterators, Go loop, Java streams, Python generators | sum |
| `lang/stream-parallel-reduce` | parallel sum of 1e8 int32 on every core | C++ `std::reduce(par)`, Rust rayon, Go goroutines, Java parallel stream, Python multiprocessing | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/stream-filter-map-reduce
metric: time
```

```bench
id: lang/stream-parallel-reduce
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench stream-filter-map-reduce               # Cajeta
./bench.sh --bench stream-filter-map-reduce --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Stream](../../../docs/stdlib/lang/stream/Stream.md)
- Source: `runtime/src/cajeta/lang/stream/Stream.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
