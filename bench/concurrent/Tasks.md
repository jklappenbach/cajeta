# Tasks

`cajeta.concurrent.Tasks` — task utilities: spawn and await. Fibers are stackful, so spawn cost and the footprint of a million parked tasks are the two numbers that place the runtime beside goroutines and virtual threads.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/task-spawn-await` | spawn and await 1e5 trivial tasks, sequentially | C++ `std::async`, Rust tokio `spawn`, Go `go` + channel, Java virtual threads, Python asyncio | count |
| `concurrent/tasks-1m-footprint` | park 1e6 tasks, spawned in steps of 1024, on 1000 channels of 1000 waiters; the row is resident memory and bytes per task | Rust tokio, Go, Java virtual threads, Python asyncio | count |

The footprint row skips with a reason under 16 GB free, and on a box whose `vm.max_map_count` is below 2 × N + 65536: a fiber is two mappings today (guard page and stack), so Phoenix's default 1,048,576 caps parked fibers near 520k. The carrier run deque is 2048 slots and aborts on overflow, which is why spawns are paced.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/task-spawn-await
metric: time
```

```bench
id: concurrent/tasks-1m-footprint
metric: memory
```

## Reproduce

```sh
cd bench
./bench.sh --bench task-spawn-await               # Cajeta
./bench.sh --bench task-spawn-await --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Tasks](../../docs/stdlib/concurrent/Tasks.md)
- Source: `runtime/src/cajeta/concurrent/Tasks.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
