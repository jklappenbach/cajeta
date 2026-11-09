# Fiber

This page measures `cajeta.concurrent.Fiber` sleeping the calling fiber. The row is wake-up latency, the number a timer-driven service lives on.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/fiber-sleep-wake` | 1e4 `sleep(1 ms)` measured against the wall clock; the row is the median overshoot | Rust tokio `sleep`, Go `time.Sleep`, Java virtual thread `Thread.sleep`, Python asyncio `sleep` | count |

A runtime timed-wait stall of about one in sixty is an open item, and the p95 column is where it shows.

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/fiber-sleep-wake
metric: time
```

## Standing

Sleep and wake takes 20 us, 5.6x Go's time.Sleep, with Java virtual threads and Python asyncio 3x slower than Cajeta and tokio 50x slower. Not yet investigated.

## Reproduce

```sh
cd bench
./bench.sh --bench fiber-sleep-wake               # Cajeta
./bench.sh --bench fiber-sleep-wake --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Fiber](../../docs/stdlib/concurrent/Fiber.md)
- Source: `runtime/src/cajeta/concurrent/Fiber.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
