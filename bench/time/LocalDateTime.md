# LocalDateTime

`cajeta.time.LocalDateTime` — a date paired with a time, no zone.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `time/localdatetime-arith` | 1e7 `plusHours` crossing day boundaries | C++ chrono, Rust `jiff` and `chrono`, Go time, Java java.time, Python datetime and `whenever` | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: time/localdatetime-arith
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench localdatetime-arith               # Cajeta
./bench.sh --bench localdatetime-arith --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [LocalDateTime](../../docs/stdlib/time/LocalDateTime.md)
- Source: `runtime/src/cajeta/time/LocalDateTime.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/time/`
