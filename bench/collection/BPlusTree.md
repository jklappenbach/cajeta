# BPlusTree

`cajeta.collection.BPlusTree` — the in-memory ordered map with B+ tree leaves in a linked chain, built for range scans.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/bplustree-insert` | insert 1e6 random int64 keys | C++ `std::map`, Rust `BTreeMap`, Java `TreeMap`, Python `sortedcontainers` | count |
| `collection/bplustree-point-lookup` | 1e7 point lookups on 1e6 keys | same | sum |
| `collection/bplustree-range` | 1e4 range scans of 1e3 keys | same | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/bplustree-insert
metric: time
```

```bench
id: collection/bplustree-point-lookup
metric: time
```

```bench
id: collection/bplustree-range
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench bplustree-insert               # Cajeta
./bench.sh --bench bplustree-insert --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [BPlusTree](../../docs/stdlib/collection/BPlusTree.md)
- Source: `runtime/src/cajeta/collection/BPlusTree.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
