# RedBlackTree

`cajeta.collection.RedBlackTree` — the ordered map, a CLRS red-black tree.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/redblacktree-insert-lookup` | insert 1e6 random int64 keys, look each up | C++ `std::map`, Rust `BTreeMap`, Go (no stdlib ordered map; `google/btree`), Java `TreeMap`, Python `sortedcontainers` | sum |
| `collection/redblacktree-range` | 1e4 range scans of 1e3 keys | same | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/redblacktree-insert-lookup
metric: time
```

```bench
id: collection/redblacktree-range
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench redblacktree-insert-lookup               # Cajeta
./bench.sh --bench redblacktree-insert-lookup --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [RedBlackTree](../../docs/stdlib/collection/RedBlackTree.md)
- Source: `runtime/src/cajeta/collection/RedBlackTree.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
