# DateTimeFormatter

`cajeta.time.DateTimeFormatter` — formatting and parsing. ISO-8601 is the shape every log line and every JSON timestamp uses.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `time/datetime-format-iso` | format 1e6 instants as ISO-8601 with nanoseconds | C++ chrono, Rust `jiff` and `chrono`, Go time, Java java.time, Python datetime and `whenever` | length |
| `time/datetime-parse-iso` | parse 1e6 ISO-8601 strings | C++ chrono, Rust `jiff` and `chrono`, Go time, Java java.time, Python datetime and `whenever` | round-trip equality |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: time/datetime-format-iso
metric: time
```

```bench
id: time/datetime-parse-iso
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench datetime-format-iso               # Cajeta
./bench.sh --bench datetime-format-iso --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [DateTimeFormatter](../../docs/stdlib/time/DateTimeFormatter.md)
- Source: `runtime/src/cajeta/time/DateTimeFormatter.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/time/`
