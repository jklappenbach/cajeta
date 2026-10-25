# Deflate (dev.cajeta.codec)

`dev.cajeta.codec` — DEFLATE and INFLATE from the codec library: the pure-cajeta streaming path that permessage-deflate uses, and the native one-shot path that gzip and content-coding use. The library's own gate is native at or above 0.8× raw zlib; these rows publish it.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `codec/deflate-stream` | `DeflateStream` over a 128 KB text-like payload, level 6 | zlib, zlib-ng, Rust `flate2`, Go compress/flate, Java `Deflater` | byte-exact inflate |
| `codec/inflate-stream` | `InflateStream` of the result | same | byte-exact |
| `codec/deflate-oneshot` | `Deflate.deflate` at levels 1, 6, 9 | same | byte-exact |
| `codec/inflate-oneshot` | `Deflate.inflate` | same | byte-exact |

Benchmark code lives in `cajeta-codec/bench/`; the rows are copied here at publish time.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: codec/deflate-stream
metric: throughput
```

```bench
id: codec/inflate-stream
metric: throughput
```

```bench
id: codec/deflate-oneshot
metric: throughput
```

```bench
id: codec/inflate-oneshot
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench deflate-stream               # Cajeta
./bench.sh --bench deflate-stream --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/codec/`
- Library: `cajeta-codec`
