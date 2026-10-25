# Encoder

`cajeta.wire.Encoder` — the bidirectional wire codec for a `T`, and the zero-copy `struct` view that reads a wire record without decoding it.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `wire/encoder-encode` | encode 1e6 100-field records | protobuf (C++ and Java), FlatBuffers, Cap'n Proto, Rust `bincode` | byte count |
| `wire/encoder-decode` | decode them back | same | field checksum |
| `wire/struct-view-read` | read three fields from 1e6 records through a `struct` view, no decode | FlatBuffers and Cap'n Proto zero-copy reads | field checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: wire/encoder-encode
metric: throughput
```

```bench
id: wire/encoder-decode
metric: throughput
```

```bench
id: wire/struct-view-read
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench encoder-encode               # Cajeta
./bench.sh --bench encoder-encode --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Encoder](../../docs/stdlib/wire/Encoder.md)
- Source: `runtime/src/cajeta/wire/Encoder.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/wire/`
