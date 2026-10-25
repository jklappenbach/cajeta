# Semaphore

`cajeta.concurrent.Semaphore` — the counting permit pool.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/semaphore-acquire-release` | 16 fibers through a 4-permit semaphore, 1e6 passes each | C++ `counting_semaphore`, Rust tokio `Semaphore`, Go buffered channel, Java `Semaphore` | count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/semaphore-acquire-release
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench semaphore-acquire-release               # Cajeta
./bench.sh --bench semaphore-acquire-release --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Semaphore](../../docs/stdlib/concurrent/Semaphore.md)
- Source: `runtime/src/cajeta/concurrent/Semaphore.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
