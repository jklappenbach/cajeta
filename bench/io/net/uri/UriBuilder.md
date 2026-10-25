# UriBuilder

`cajeta.io.net.uri.UriBuilder` — URI construction.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/uri-build` | build 1e6 URIs from parts with encoding | same libraries | length sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/uri-build
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench uri-build               # Cajeta
./bench.sh --bench uri-build --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [UriBuilder](../../../../docs/stdlib/io/net/uri/UriBuilder.md)
- Source: `runtime/src/cajeta/io/net/uri/UriBuilder.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
