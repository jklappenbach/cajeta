# LocalDateTime

`cajeta.time.LocalDateTime` is a date paired with a time, no zone.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `time/localdatetime-arith` | 1e7 `plusHours` crossing day boundaries | C++ chrono, Rust `jiff` and `chrono`, Go time, Java java.time, Python datetime and `whenever` | checksum |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: time/localdatetime-arith
metric: time
```

## Standing

Arithmetic is 2.5x of chrono and 1.2x to 1.7x of jiff, Go and java.time. Not yet investigated.

## Reproduce

```sh
cd bench
./bench.sh --bench localdatetime-arith               # Cajeta
./bench.sh --bench localdatetime-arith --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [LocalDateTime](../../docs/stdlib/time/LocalDateTime.md)
- Source: `runtime/src/cajeta/time/LocalDateTime.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/time/`
