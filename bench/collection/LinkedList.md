# LinkedList

`cajeta.collection.LinkedList` — the doubly-linked list.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/linkedlist-insert-traverse` | insert 1e6 at the head, traverse twice | C++ `std::list`, Rust `LinkedList`, Go `container/list`, Java `LinkedList`, Python deque | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/linkedlist-insert-traverse
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench linkedlist-insert-traverse               # Cajeta
./bench.sh --bench linkedlist-insert-traverse --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [LinkedList](../../docs/stdlib/collection/LinkedList.md)
- Source: `runtime/src/cajeta/collection/LinkedList.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
