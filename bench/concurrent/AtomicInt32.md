# AtomicInt32

`cajeta.concurrent.AtomicInt32` is the lock-free atomic `int32` cell.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/atomic-fetchadd-32` | 1e8 `fetchAdd(1)` on one thread | C++, Rust `AtomicI32`, Go `atomic.AddInt32`, Java `AtomicInteger` | final value |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/atomic-fetchadd-32
metric: time
```

## Standing

Fetch-add is 1.1x of Go's atomic.Int32 and within 10% of Rust, C++ and Java: parity.

## Reproduce

```sh
cd bench
./bench.sh --bench atomic-fetchadd-32               # Cajeta
./bench.sh --bench atomic-fetchadd-32 --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [AtomicInt32](../../docs/stdlib/concurrent/AtomicInt32.md)
- Source: `runtime/src/cajeta/concurrent/AtomicInt32.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
