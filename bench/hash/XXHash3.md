# XXHash3

`cajeta.hash.XXHash3` — the 64- and 128-bit XXH3. The small-input rows are the per-key cost a hash map pays; the 256 KB rows are bandwidth, read against the reference C implementation's own number.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `hash/xxhash3` | XXH3-64 of 32-byte keys, 1e7 times | xxHash reference C, Rust `xxhash-rust`, Go `zeebo/xxh3`, Java `zero-allocation-hashing`, Python `xxhash` | digest |
| `hash/xxhash3-256k` | XXH3-64 of a 256 KB buffer, 1e4 times | same | digest |
| `hash/xxhash3_128` | XXH3-128 of 32-byte keys | same | digest |
| `hash/xxhash3_128-256k` | XXH3-128 of 256 KB | same | digest |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: hash/xxhash3
metric: throughput
```

```bench
id: hash/xxhash3-256k
metric: throughput
```

```bench
id: hash/xxhash3_128
metric: throughput
```

```bench
id: hash/xxhash3_128-256k
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench xxhash3               # Cajeta
./bench.sh --bench xxhash3 --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [XXHash3](../../docs/stdlib/hash/XXHash3.md)
- Source: `runtime/src/cajeta/hash/XXHash3.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/hash/`
