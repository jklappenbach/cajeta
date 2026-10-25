# Buffer

`cajeta.io.Buffer` — the byte buffer behind every reader and writer.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/buffer-fill` | fill 16 MB byte by byte, 1e2 times (through the array the `Buffer` wraps; it has no store methods) | C++ `std::vector<uint8_t>`, Rust `Vec<u8>`, Go `[]byte`, Java `ByteBuffer`, Python `bytearray` | checksum |
| `io/buffer-read-primitives` | read 1e8 little-endian int32 and float64 values (through `loadU64`; there are no typed loads) | same | checksum |
| `io/buffer-slice` | 1e7 sub-buffer views (array slices; `Buffer` has no slice) | same | sum of first bytes |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/buffer-fill
metric: throughput
```

```bench
id: io/buffer-read-primitives
metric: throughput
```

```bench
id: io/buffer-slice
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench buffer-fill               # Cajeta
./bench.sh --bench buffer-fill --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Buffer](../../docs/stdlib/io/Buffer.md)
- Source: `runtime/src/cajeta/io/Buffer.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
