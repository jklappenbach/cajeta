# AtomicInt64

`cajeta.concurrent.AtomicInt64` — the lock-free atomic `int64` cell. The single-threaded fetch-add loop is the hardware `lock xadd` floor; the first suite found 35% of overhead around it.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/atomic-fetchadd` | 1e8 `fetchAdd(1)` on one thread | C++ `std::atomic`, Rust `AtomicI64`, Go `atomic.AddInt64`, Java `AtomicLong`, Python (GIL; noted) | final value |
| `concurrent/atomic-cas` | 1e8 compare-and-swap, no contention | same | final value |
| `concurrent/atomic-contended-16` | 16 fibers each doing 1e6 fetch-adds on one cell | same, 16 threads | final value |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/atomic-fetchadd
metric: time
```

```bench
id: concurrent/atomic-cas
metric: time
```

```bench
id: concurrent/atomic-contended-16
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench atomic-fetchadd               # Cajeta
./bench.sh --bench atomic-fetchadd --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [AtomicInt64](../../docs/stdlib/concurrent/AtomicInt64.md)
- Source: `runtime/src/cajeta/concurrent/AtomicInt64.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
