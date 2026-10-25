# Mutex

`cajeta.concurrent.Mutex` — fused mutual exclusion and the data it protects. Uncontended lock and unlock is the floor; four and sixteen contending fibers show the scheduler.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/mutex-uncontended` | 1e8 lock-increment-unlock on one fiber | C++ `std::mutex`, Rust `Mutex`, Go `sync.Mutex`, Java `ReentrantLock`, Python `threading.Lock` | final value |
| `concurrent/mutex-contended-4` | 4 fibers each 1e6 increments | same, 4 threads | final value |
| `concurrent/mutex-contended-16` | 16 fibers each 1e6 increments | same, 16 threads | final value |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/mutex-uncontended
metric: time
```

```bench
id: concurrent/mutex-contended-4
metric: time
```

```bench
id: concurrent/mutex-contended-16
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench mutex-uncontended               # Cajeta
./bench.sh --bench mutex-uncontended --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Mutex](../../docs/stdlib/concurrent/Mutex.md)
- Source: `runtime/src/cajeta/concurrent/Mutex.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
