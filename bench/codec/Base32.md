# Base32

`cajeta.codec.Base32` — Base32 encode and decode.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `codec/base32-encode` | encode 1 MB, 1e3 times | Rust `data-encoding`, Go encoding/base32, Java commons-codec, Python `base64.b32encode` | length |
| `codec/base32-decode` | decode the result | same | equality |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: codec/base32-encode
metric: throughput
```

```bench
id: codec/base32-decode
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench base32-encode               # Cajeta
./bench.sh --bench base32-encode --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Base32](../../docs/stdlib/codec/Base32.md)
- Source: `runtime/src/cajeta/codec/Base32.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/codec/`
