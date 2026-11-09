# Semaphore

`cajeta.concurrent.Semaphore` is the counting permit pool.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/semaphore-acquire-release` | 16 fibers through a 4-permit semaphore, 1e6 passes each | C++ POSIX `sem_t` (libstdc++ 13's `counting_semaphore` hangs at this shape and is recorded as a skip), Rust tokio `Semaphore`, Go buffered channel, Java `Semaphore` | count |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/semaphore-acquire-release
metric: time
```

## Standing

Acquire and release is 10.9x behind java.util.concurrent, 2x behind tokio's semaphore and 5x behind a POSIX sem_t. The uncontended path is [Lock](Lock.md)'s; not yet investigated.

## Reproduce

```sh
cd bench
./bench.sh --bench semaphore-acquire-release               # Cajeta
./bench.sh --bench semaphore-acquire-release --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Semaphore](../../docs/stdlib/concurrent/Semaphore.md)
- Source: `runtime/src/cajeta/concurrent/Semaphore.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
