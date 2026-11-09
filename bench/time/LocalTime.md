# LocalTime

`cajeta.time.LocalTime` is time-of-day with no date or zone.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `time/localtime-arith` | 1e7 `plusSeconds` and comparisons | C++ chrono, Rust `jiff` and `chrono`, Go time, Java java.time, Python datetime and `whenever` | checksum |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: time/localtime-arith
metric: time
```

## Standing

Arithmetic is 6.1x of chrono and 2x to 3.5x of jiff, Go and java.time. Not yet investigated, with [Duration](Duration.md).

## Reproduce

```sh
cd bench
./bench.sh --bench localtime-arith               # Cajeta
./bench.sh --bench localtime-arith --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [LocalTime](../../docs/stdlib/time/LocalTime.md)
- Source: `runtime/src/cajeta/time/LocalTime.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/time/`
