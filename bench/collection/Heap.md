# Heap

`cajeta.collection.Heap` — the array-backed binary heap and priority queue.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/heap-sort` | push 1e6 random int32, pop all in order | C++ `priority_queue`, Rust `BinaryHeap`, Go `container/heap`, Java `PriorityQueue`, Python heapq | non-decreasing |
| `collection/heap-push-pop` | 1e7 alternating push and pop on a 1e3 heap | same | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/heap-sort
metric: time
```

```bench
id: collection/heap-push-pop
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench heap-sort               # Cajeta
./bench.sh --bench heap-sort --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Heap](../../docs/stdlib/collection/Heap.md)
- Source: `runtime/src/cajeta/collection/Heap.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
