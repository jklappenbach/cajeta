# ByteBuffer

`cajeta.io.net.ByteBuffer` — the network byte buffer with big-endian accessors.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/bytebuffer-put-get` | 1e8 put and get of int32 and int64 built from `put(int8)` and `at()` (there are no typed big-endian accessors yet) | Java `ByteBuffer`, Rust `bytes`, Go `encoding/binary`, C++ | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/bytebuffer-put-get
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench bytebuffer-put-get               # Cajeta
./bench.sh --bench bytebuffer-put-get --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [ByteBuffer](../../../docs/stdlib/io/net/ByteBuffer.md)
- Source: `runtime/src/cajeta/io/net/ByteBuffer.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
