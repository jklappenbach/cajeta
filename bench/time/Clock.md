# Clock

`cajeta.time.Clock` — the static what-time-is-it surface. `Clock.now()` is wall-clock time at millisecond resolution; the competitors read monotonic nanosecond clocks, so this row compares a different call and says so.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `time/clock-now` | 1e7 `Clock.now()` (wall clock, milliseconds) | C++ `steady_clock`, Rust `Instant::now`, Go `time.Now`, Java `Instant.now`, Python `time.perf_counter_ns` (monotonic, nanoseconds) | monotonic |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: time/clock-now
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench clock-now               # Cajeta
./bench.sh --bench clock-now --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Clock](../../docs/stdlib/time/Clock.md)
- Source: `runtime/src/cajeta/time/Clock.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/time/`
