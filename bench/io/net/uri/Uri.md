# Uri

`cajeta.io.net.uri.Uri` — URI parsing.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/uri-parse` | parse 1e6 URIs with query strings and percent-encoding | Rust `url`, Go `net/url`, Java `URI`, Python `urllib.parse`, C++ `boost::url` | component checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/uri-parse
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench uri-parse               # Cajeta
./bench.sh --bench uri-parse --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Uri](../../../../docs/stdlib/io/net/uri/Uri.md)
- Source: `runtime/src/cajeta/io/net/uri/Uri.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
