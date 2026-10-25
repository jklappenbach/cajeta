# RwLock

`cajeta.concurrent.RwLock` — the reader-writer lock for read-heavy shared state.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/rwlock-read-heavy` | 16 fibers, 90% reads 10% writes, 1e6 operations each | C++ `shared_mutex`, Rust `RwLock`, Go `sync.RWMutex`, Java `ReentrantReadWriteLock` | final value |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/rwlock-read-heavy
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench rwlock-read-heavy               # Cajeta
./bench.sh --bench rwlock-read-heavy --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [RwLock](../../docs/stdlib/concurrent/RwLock.md)
- Source: `runtime/src/cajeta/concurrent/RwLock.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
