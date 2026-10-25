# Server

`cajeta-http` — the TechEmpower plaintext and JSON shapes without a database, over HTTP/1.1 keep-alive and HTTP/2, at 64 and 512 connections. `oha` drives; `wrk` cross-checks one row. Every competitor's response is byte-compared before load.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `http/plaintext-h1` | `GET /plaintext`, HTTP/1.1; variants 64, 512 connections | Go `net/http`, Rust `axum`, Java Netty, Node.js `http`, Python `uvicorn` | zero errors, body equal |
| `http/json-h1` | `GET /json`, HTTP/1.1 | Go `net/http`, Rust `axum`, Java Netty, Node.js `http`, Python `uvicorn` | zero errors, body equal |
| `http/plaintext-h2` | HTTP/2 multiplexed | Go `net/http`, Rust `axum`, Java Netty, Node.js `http`, Python `uvicorn` | zero errors |
| `http/json-h2` | HTTP/2 multiplexed | Go `net/http`, Rust `axum`, Java Netty, Node.js `http`, Python `uvicorn` | zero errors |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: http/plaintext-h1
metric: throughput
```

```bench
id: http/json-h1
metric: throughput
```

```bench
id: http/plaintext-h2
metric: throughput
```

```bench
id: http/json-h2
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench plaintext-h1               # Cajeta
./bench.sh --bench plaintext-h1 --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/http/`
- Library: `cajeta-http`, benchmark code in `cajeta-http/bench/`
