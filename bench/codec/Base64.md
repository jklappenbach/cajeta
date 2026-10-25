# Base64

`cajeta.codec.Base64` — Base64 encode and decode, read against the SIMD implementations as well as the standard libraries.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `codec/base64-encode` | encode 1 MB, 1e3 times | simdutf, Rust `base64`, Go encoding/base64, Java `Base64`, Python `base64` | length |
| `codec/base64-decode` | decode the result | same | equality |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: codec/base64-encode
metric: throughput
```

```bench
id: codec/base64-decode
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench base64-encode               # Cajeta
./bench.sh --bench base64-encode --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Base64](../../docs/stdlib/codec/Base64.md)
- Source: `runtime/src/cajeta/codec/Base64.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/codec/`
