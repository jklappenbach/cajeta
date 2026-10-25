# CompressStream

`cajeta.wire.CompressStream` — a compressing wire stream, round-tripped.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `wire/compressstream-roundtrip` | write and read back 64 MB of records through the stream | zlib and zstd streams in C++, Rust, Go, Java | byte equality |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: wire/compressstream-roundtrip
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench compressstream-roundtrip               # Cajeta
./bench.sh --bench compressstream-roundtrip --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [CompressStream](../../docs/stdlib/wire/CompressStream.md)
- Source: `runtime/src/cajeta/wire/CompressStream.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/wire/`
