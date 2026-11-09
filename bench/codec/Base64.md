# Base64

`cajeta.codec.Base64` encodes and decodes Base64. The rows are read against the SIMD implementations as well as the standard libraries.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `codec/base64-encode` | encode 1 MB, 1e3 times | simdutf, Rust `base64`, Go encoding/base64, Java `Base64`, Python `base64` | length |
| `codec/base64-decode` | decode the result | same | equality |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: codec/base64-encode
metric: throughput
```

```bench
id: codec/base64-decode
metric: throughput
```

## Standing

The weakest rows of the package. Encoding is 330x and decoding 60x behind simdutf, which is SIMD throughout, and 10x to 25x behind the scalar libraries (Rust base64, Go, Java, Python). simdutf is SIMD end to end; a SIMD codec is the fix and is not yet scheduled.

## Reproduce

```sh
cd bench
./bench.sh --bench base64-encode               # Cajeta
./bench.sh --bench base64-encode --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Base64](../../docs/stdlib/codec/Base64.md)
- Source: `runtime/src/cajeta/codec/Base64.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/codec/`
