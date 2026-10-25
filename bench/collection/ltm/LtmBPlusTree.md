# LtmBPlusTree

`cajeta.collection.ltm.LtmBPlusTree` — the disk-backed, larger-than-memory ordered map. The competitors are embedded key-value stores, which is the role it plays.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/ltm-insert` | insert random 16-byte keys with 64-byte values under a page budget; the page shape is 1e7 keys and 256 MB, the default run is 1e6 and 16 MB (see the note) | RocksDB and LMDB through their Rust and Java bindings | count |
| `collection/ltm-lookup` | random point lookups after the insert (1e6 at page shape, 1e5 by default) | same | sum of value lengths |

Skips with a reason when the disk budget is unavailable; the row records the storage device. The default is a tenth of the page shape because `LtmPager` scans its whole frame table on every page touch (44 µs an insert at 4096 frames): at 65536 frames one trial would take hours. `CAJETA_BENCH_LTM_N`, `_LOOKUPS` and `_BUDGET_MB` restore the page shape and the competitors read the same variables.

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
