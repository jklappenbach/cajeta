# ZonedDateTime

`cajeta.time.ZonedDateTime` — a date-time with a fixed UTC offset; the row applies a region zone with transitions, which is where the tz database is paid for.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `time/zoneddatetime-region` | 1e6 conversions of an `Instant` into America/Los_Angeles across DST boundaries | Rust `jiff`, Go `time.In`, Java `ZonedDateTime`, Python `zoneinfo`, C++ `std::chrono::zoned_time` | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: time/zoneddatetime-region
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench zoneddatetime-region               # Cajeta
./bench.sh --bench zoneddatetime-region --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [ZonedDateTime](../../docs/stdlib/time/ZonedDateTime.md)
- Source: `runtime/src/cajeta/time/ZonedDateTime.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/time/`
