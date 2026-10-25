# Lock

`cajeta.concurrent.Lock` — the no-data RAII gate.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/lock-uncontended` | 1e8 acquire and release on one fiber | C++ `std::mutex`, Rust `Mutex<()>`, Go `sync.Mutex`, Java `ReentrantLock` | count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/lock-uncontended
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench lock-uncontended               # Cajeta
./bench.sh --bench lock-uncontended --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Lock](../../docs/stdlib/concurrent/Lock.md)
- Source: `runtime/src/cajeta/concurrent/Lock.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
