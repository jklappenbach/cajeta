# Client

`cajeta-http` — `HttpClient` throughput and connection reuse against a local server.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `http/client-get-throughput` | 1e5 GETs over 16 reused connections | Go `http.Client`, Rust `reqwest`, Java `HttpClient`, Node `fetch`, Python `httpx` | status and body |
| `http/client-connection-reuse` | 1e4 GETs with and without keep-alive; the row is the ratio | same | status |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: http/client-get-throughput
metric: throughput
```

```bench
id: http/client-connection-reuse
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench client-get-throughput               # Cajeta
./bench.sh --bench client-get-throughput --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/http/`
- Library: `cajeta-http`
