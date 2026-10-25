# LtmBPlusTree

`cajeta.collection.ltm.LtmBPlusTree` — the disk-backed, larger-than-memory ordered map. The competitors are embedded key-value stores, which is the role it plays.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/ltm-insert` | insert 1e7 random 16-byte keys with 64-byte values under a 256 MB page budget | RocksDB and LMDB through their Rust and Java bindings | count |
| `collection/ltm-lookup` | 1e6 random point lookups after the insert | same | sum of value lengths |

Skips with a reason when the disk budget is unavailable; the page records the storage device of the run.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/ltm-insert
metric: time
```

```bench
id: collection/ltm-lookup
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench ltm-insert               # Cajeta
./bench.sh --bench ltm-insert --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [LtmBPlusTree](../../../docs/stdlib/collection/ltm/LtmBPlusTree.md)
- Source: `runtime/src/cajeta/collection/ltm/LtmBPlusTree.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
