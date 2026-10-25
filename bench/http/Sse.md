# Server-sent events

`cajeta-http` — fan-out to a thousand subscribers.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `http/sse-fanout-1k` | 1e3 clients, 1e3 events each | Go `net/http`, Rust `axum`, Java Netty, Node.js `http`, Python `uvicorn` | event count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: http/sse-fanout-1k
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench sse-fanout-1k               # Cajeta
./bench.sh --bench sse-fanout-1k --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/http/`
- Library: `cajeta-http`
