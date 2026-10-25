# Channel

`cajeta.concurrent.Channel` — the bounded multi-producer multi-consumer queue, measured with the crossbeam-channel harness shapes: 5M messages, four threads.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/channel-spsc` | one producer, one consumer, 5e6 messages, capacity 1e3 | Rust crossbeam and std mpsc, Go channel, Java `ArrayBlockingQueue`, C++ moodycamel `ConcurrentQueue` bounded by its `LightweightSemaphore` | sum |
| `concurrent/channel-mpsc` | four producers, one consumer | same | sum |
| `concurrent/channel-mpmc` | four producers, four consumers | same; Rust std mpsc has one receiver and skips | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/channel-spsc
metric: time
```

```bench
id: concurrent/channel-mpsc
metric: time
```

```bench
id: concurrent/channel-mpmc
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench channel-spsc               # Cajeta
./bench.sh --bench channel-spsc --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Channel](../../docs/stdlib/concurrent/Channel.md)
- Source: `runtime/src/cajeta/concurrent/Channel.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
