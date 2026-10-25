# LocalDate

`cajeta.time.LocalDate` — the calendar date with no time or zone. Date arithmetic is the proleptic-Gregorian loop every date library must get right and fast.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `time/time-localdate-arith` | 1e7 `plusDays`, `plusMonths`, day-of-week | C++ chrono, Rust `jiff` and `chrono`, Go time, Java java.time, Python datetime and `whenever` | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: time/time-localdate-arith
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench time-localdate-arith               # Cajeta
./bench.sh --bench time-localdate-arith --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [LocalDate](../../docs/stdlib/time/LocalDate.md)
- Source: `runtime/src/cajeta/time/LocalDate.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/time/`
